"""Adapter regression checks; optional protobuf check uses the terminal schema."""
import asyncio
import importlib
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "examples"))
import file_feed as feed
from dataframe_demo import demo_frame
import websockets

class DataContract(unittest.TestCase):
    def test_reproducible_dataframe_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            csv, parquet = Path(directory)/"x.csv", Path(directory)/"x.parquet"
            frame = demo_frame()
            frame.to_csv(csv,index=False);frame.to_parquet(parquet,index=False)
            a,b=feed.load(csv),feed.load(parquet)
            self.assertEqual(a,b)
            self.assertEqual(len(a),7200)
            self.assertAlmostEqual(sum(t[2] for t in a),287.94)

    def test_timestamp_precision_and_timezone(self):
        for v in ("1700000000.123", "1700000000123", "1700000000123999", "1700000000123999999"):
            self.assertEqual(feed._ts_ms(v),1700000000123)
        self.assertEqual(feed._ts_ms("2023-11-15T11:13:20.123+13:00"),1700000000123)
        for v in ("2023-11-14T22:13:20", "nan", "inf", "-1"):
            with self.assertRaises(ValueError):feed._ts_ms(v)

    def test_no_invented_side_or_negative_size(self):
        self.assertFalse(feed._is_buy(True,True))
        for v in (None,"unknown","","long","ask"):
            with self.assertRaises(ValueError):feed._is_buy(v)
        row={"timestamp":1700000000000,"price":100,"qty":1,"side":"buy"}
        for patch in ({"qty":-1},{"price":math.nan},{"side":None}):
            with self.assertRaises(ValueError):feed._load_rows([{**row,**patch}],"auto")
        del row['side']
        with self.assertRaisesRegex(ValueError,"side"):feed._load_rows([row],"auto")

    def test_partial_minutes_do_not_become_history(self):
        ts=1788739200000
        trades=[(ts,100,2,True),(ts+1,101,3,False),(ts+60000,999,99,True)]
        minutes=feed.minute_levels(trades,ts,ts+60000)
        self.assertEqual(sum(sum(v[:2]) for levels in minutes.values() for v in levels.values()),5)
        self.assertFalse(feed.minute_levels(trades,ts+1,ts+60000))
        self.assertFalse(feed.minute_levels(trades,ts,ts+59999))

class WireContract(unittest.IsolatedAsyncioTestCase):
    async def test_terminal_decode_and_subscription_lifecycle(self):
        try: pb=importlib.import_module('messages_pb2')
        except ImportError:self.skipTest('generate terminal messages_pb2 with protoc; see tests/examples/README.md')
        ts=1788739200000
        trades=[(ts,100,2,True),(ts+1,101,3,False),(ts+60000,102,4,True),(ts+120000,103,5,False)]
        resume={"cursor":2}
        server=await websockets.serve(lambda ws:feed.serve(ws,trades,2,1,'binancef','btcusdt',resume),'127.0.0.1',0)
        port=server.sockets[0].getsockname()[1]
        async with server:
            async with websockets.connect(f'ws://127.0.0.1:{port}') as ws:
                async def request(method,**data):
                    await ws.send(json.dumps({'method':method,'data':{'pair':{'exchange':'binancef','symbol':'btcusdt'},**data}}))
                async def receive():
                    return pb.WSPayload.FromString(await asyncio.wait_for(ws.recv(),2))
                await ws.send('[]')
                await ws.send(json.dumps({'data': [1]}))
                await request('get_footprint_history',start_time=ts,end_time=ts+999999)
                p=await receive();self.assertEqual(p.stream,17);self.assertEqual(p.timeframe,60000)
                u=pb.TickVolumeUpdate.FromString(p.data)
                self.assertEqual((u.total_volume,u.buy_volume,u.sell_volume,u.end_time),(5,2,3,ts+60000))
                levels=pb.TickVolumeLevels.FromString(u.levels_data)
                self.assertEqual([l.price for l in levels.levels],[100,101])
                self.assertEqual((await receive()).data,b'')
                await request('get_volume_profile',start_time=ts,end_time=ts+999999,tick_per_row=1)
                vp=pb.VolumeProfileResponse.FromString((await receive()).data)
                self.assertEqual((vp.total_volume,vp.poc,vp.val,vp.vah),(5,101,100,101))
                await request('get_historical_candles',timeframe=60,count=1)
                candles=pb.Candles.FromString((await receive()).data)
                self.assertEqual(candles.values[0].close,101)
                self.assertEqual((candles.values[0].vbuy,candles.values[0].vsell),(2,3))
                self.assertTrue(candles.values[0].final)
                await request('subscribe',stream=1)
                tr=pb.Trade.FromString((await receive()).data)
                self.assertEqual(tr.timestamp_ms,ts+60000)
                await request('unsubscribe',stream=1)
                # Reconnect resumes forward; never replay earlier rows into retained client state.
            async with websockets.connect(f'ws://127.0.0.1:{port}') as ws:
                await request('subscribe',stream=1)
                self.assertEqual(pb.Trade.FromString((await receive()).data).timestamp_ms,ts+120000)
                await request('get_historical_candles',timeframe=60,count=1)
                last=pb.Candles.FromString((await receive()).data).values[0]
                self.assertEqual((last.close,last.vsell),(103,5))
                self.assertFalse(last.final)

if __name__=='__main__':unittest.main()
