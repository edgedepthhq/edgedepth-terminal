# Browser replay lifecycle regression

Run this with the candidate terminal and a live local gateway. The automated
stream lifecycle test checks queued stats delivery after callback teardown;
this browser sequence additionally exercises actual ChartWidget destruction.

1. Open the normal live terminal with chart, DOM and trade tape. Wait for real
   stats and trades. Note the symbol and current wall-clock timestamps.
2. Use Replay > Open Replay Library > TUT > Replay. Do not substitute a `?pack=`
   URL: that mode intentionally has no live socket and cannot test this bug.
3. Verify the replay chart, DOM and tape identify TUT and use recorded prices
   and dates. Pause; wait at least 30 seconds; verify those prices and timestamps
   remain in replay while the live gateway remains available.
4. Exit with the replay bar's X. Wait for multiple new live trade frames and a
   stats update. Verify the original chart/DOM/tape are restored and moving.
   Inspect the console for memory errors; restored panels alone are not a pass.
5. Repeat enter/exit, including a live symbol switch and a timeframe change.
   Inspect the console again.

Failure reproduced on the published acda5c5 image on 8 September 2026: replay
exit restored panels then a stats callback wrote through a destroyed chart.
The callback owner must remain the manager that accepted the subscription,
not the shared AppContext after it has swapped to replay or back to live.
