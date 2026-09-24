#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/exposure_history.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace exposure;
Json load(const std::string& path) { std::ifstream f(path);assert(f.good());return Json::parse(f); }
Window window_for(const Json& p) {
    return Window({p["venue"],p["market"],p["scenario_id"]},p["from_ms"],p["to_ms"]);
}
int main(int argc,char** argv) {
    assert(argc==3);
    const std::string dir=argv[1];
    const auto first=load(dir+"/anchored-1.json"),second=load(dir+"/anchored-2.json");
    auto window=window_for(first);
    assert(window.accept(first));assert(!window.view());
    assert(window.cursor()["after_sequence"]=="20");
    assert(window.accept(first));assert(!window.view()); // Identical page retry.
    assert(window.accept(second));assert(window.complete());
    assert(window.view()->at(21500).publication->sequence==21); // Same-ms tie crosses page boundary.
    assert(std::string_view(window.view()->at(5500).status)=="missing");
    assert(std::string_view(window.view()->at(5499).status)=="missing");
    assert(window.view()->at(22501).reason=="outside_loaded_coverage");
    for (const auto* page:{&first,&second}) for (const auto& raw:(*page)["publications"]) {
        const auto decoded=Json::parse(raw.get<std::string>());
        assert(window.view()->records().at(decoded["sequence"].get<uint64_t>()).raw==raw.get_ref<const std::string&>());
    }
    for (int64_t clock:{22500,21500,5500,18000,6000,22500}) {
        const auto state=window.view()->at(clock);assert(state.publication && state.publication->published<=clock);
        rectangles(*window.view(),clock,[&](auto&,auto&,int64_t begin,int64_t end){assert(begin>=5500 && end<=clock && end<=22500);});
    }
    for (const std::string name:{"before-capture","before-capture-to-first","missing-anchor","expired-anchor"}) {
        const auto page=load(dir+"/"+name+".json");auto w=window_for(page);assert(w.accept(page));assert(w.view());
        const auto state=w.view()->at(page["from_ms"].get<int64_t>());
        const std::string expected=name=="missing-anchor"?"missing":name=="expired-anchor"?"expired":"before_capture";
        assert(state.status==expected);
    }
    for (int mutation=0;mutation<8;++mutation) {
        auto w=window_for(first);auto broken=first;
        if (mutation==0) broken["version"]="unknown";
        if (mutation==1) broken["through_sequence"]="022";
        if (mutation==2) broken["complete"]=true;
        if (mutation==3) broken["anchor_sequence"]=5;
        if (mutation==4) broken["market"]="ETHUSDT";
        if (mutation==5) broken["publications"].erase(broken["publications"].begin());
        if (mutation==6) broken["next_sequence"]="21";
        if (mutation==7) broken["from_ms"]=5499;
        assert(!w.accept(broken));assert(!w.view());
    }
    auto out_of_order=window_for(first);assert(!out_of_order.accept(second));
    auto changed=window_for(first);assert(changed.accept(first));auto bad=second;bad["through_sequence"]="23";assert(!changed.accept(bad));assert(!changed.view());
    uint64_t seq=0;
    assert(sequence_string(Json{{"s","9007199254740993"}},"s",seq)&&seq==9007199254740993ULL);
    assert(!sequence_string(Json{{"s","9223372036854775808"}},"s",seq));
    const Identity identity{first["venue"],first["market"],first["scenario_id"]};
    auto response=[](const Json& request,const Json& page){return Json{{"type","exposure_v2"},{"request_id",request["data"]["request_id"]},{"mode","replay"},{"page",page}};};
    // Zooming to an old two-hour interval must request that interval, not now.
    const int64_t now=1800000000000LL,old_end=now-7*86400000LL;
    const int64_t old_start=exact_window_start(old_end-2*3600000,old_end);
    assert(old_start==old_end-2*3600000);
    assert(exact_window_start(old_end-8*3600000,old_end)==old_end-4*3600000);
    History historical;
    const auto old_request=historical.poll(identity,old_start,now,false,1000,old_end);
    assert(old_request["data"]["from_ms"]==old_start && old_request["data"]["to_ms"]==old_end);
    const auto narrow=historical.poll(identity,old_start,now+1000,false,1100,old_end-60000);
    assert(!narrow.is_null() && narrow["data"]["to_ms"]==old_end-60000);
    History::receive({{"request_id",old_request["data"]["request_id"]},{"error","pro_required"}},false);
    assert(historical.status().find("Loading")!=std::string::npos); // Cancelled reply stays ignored.
    const auto future=historical.poll(identity,now-60000,now,false,1200,now+60000);
    assert(future["data"]["to_ms"]==now); // Playback/live ceiling remains authoritative.
    History history;
    const auto a=history.poll(identity,5500,22500,true,1000);assert(!a.is_null());
    History::receive(response(a,first),false);assert(!history.view()); // Wrong lane cannot claim the request.
    History::receive(response(a,first),true);assert(!history.view());
    // The next validated page can be requested on the very next update.
    const auto b=history.poll(identity,5500,22500,true,1001);assert(b["data"]["cursor"]["after_sequence"]=="20");
    assert(a["data"]["request_id"]!=b["data"]["request_id"]);
    History::receive(response(b,second),true);assert(history.view());
    assert(history.state(21500).publication->sequence==21);
    const auto* retained=history.view();
    const auto refresh=history.poll(identity,5500,23000,true,7000);
    assert(!refresh.is_null() && history.view()==retained && history.covered_to()==22500);
    auto next_first=first;next_first["to_ms"]=23000;
    auto next_second=second;next_second["to_ms"]=23000;
    History::receive(response(refresh,next_first),true);assert(history.view()==retained);
    const auto refresh_tail=history.poll(identity,5500,23000,true,7250);
    History::receive(response(refresh_tail,next_second),true);
    assert(history.view() && history.covered_to()==23000);
    retained=history.view();
    const auto pan=history.poll(identity,6000,23000,true,7500);
    assert(!pan.is_null() && history.view()==retained);
    history.poll(identity,5500,22000,true,7600);assert(!history.view()); // Rewind clears both owners.
    History::receive(response(pan,second),true);assert(!history.view());
    history.reset();
    const auto reload=history.poll(identity,5500,22500,true,8000);
    History::receive(response(reload,first),true);
    const auto tail=history.poll(identity,5500,22500,true,8250);
    History::receive(response(tail,second),true);assert(history.view());
    const auto failed=history.poll(identity,5500,23000,true,14000);
    History::receive({{"request_id",failed["data"]["request_id"]},{"error","pro_required"}},true);
    assert(!history.view());
    History::invalidate_all("dispatch gap");assert(!history.view());
    History::receive(response(b,second),true);assert(!history.view());
    const auto c=history.poll(identity,5500,22500,true,2000);
    History::receive({{"request_id",c["data"]["request_id"]},{"error","busy"}},true);
    assert(history.poll(identity,5500,22500,true,2500).is_null());
    const auto d=history.poll(identity,5500,22500,true,3000);assert(!d.is_null());
    auto wrong=response(d,first);wrong["mode"]="live";History::receive(wrong,true);assert(!history.view());
    history.reset();const auto late=history.poll(identity,5500,22500,true,4000);
    history.poll(identity,5500,18000,true,4050); // Backward seek invalidates previous request.
    History::receive(response(late,first),true);assert(!history.view());
    history.poll(identity,5500,18000,true,15000);assert(!history.view());assert(history.status().find("timed out")!=std::string::npos);
    size_t public_bands=0;
    for (const std::string market:{"BTCUSDT","ETHUSDT","SOLUSDT"}) {
        const auto page=load(std::string(argv[2])+"/"+market+"-page.json");
        std::ifstream input(std::string(argv[2])+"/"+market+"-publication.json");
        assert(input.good());const std::string raw((std::istreambuf_iterator<char>(input)),{});
        const Identity id{page["venue"],page["market"],page["scenario_id"]};
        Publication p;assert(decode(raw,id,p));assert(!p.bands.empty());public_bands+=p.bands.size();
        assert(page["publications"][0].get<std::string>()==raw);
        auto w=window_for(page);assert(w.accept(page));assert(w.view());
        assert(w.view()->at(p.published).publication->raw==raw);
        assert(w.view()->at(p.published+1).reason=="outside_loaded_coverage");
        // A direct recorded-publication lifetime is distinct from the instant page.
        Timeline lifetime(id,p.sequence);assert(lifetime.ingest(raw));assert(lifetime.ingest(raw));
        for (const int64_t clock:{p.published,p.expires-1,p.published-1,p.expires,p.published}) {
            const auto state=lifetime.at(clock);
            assert(std::string_view(state.status)==(clock<p.published?"before_capture":clock>=p.expires?"expired":"available"));
        }
        size_t cells=0;
        rectangles(lifetime,p.expires,[&](const Publication& actual,const Band& band,int64_t begin,int64_t end) {
            assert(actual.raw==raw && begin==p.published && end==p.expires);
            const auto& source=actual.frame["bands"][cells++];
            assert(source["low"].get<double>()==band.low && source["high"].get<double>()==band.high && source["mass"].get<double>()==band.mass);
        });assert(cells==p.bands.size());
        for (int mutation=0;mutation<7;++mutation) {
            auto bad_public=Json::parse(raw);auto& provenance=bad_public["frame"]["public_provenance"];
            if (mutation==0) provenance["mode"]="reconstructed";
            if (mutation==1) provenance["source_manifest_sha256"]="bad";
            if (mutation==2) provenance["assumptions"]["scenario"]["buffer"]=.08;
            if (mutation==3) provenance["assumptions"]["available_at_ms"]=p.published+1;
            if (mutation==4) provenance["evaluated_at_ms"]=p.published+1;
            if (mutation==5) provenance["assumptions"]["valid_until_ms"]=p.expires-1;
            if (mutation==6) bad_public["frame"].erase("public_provenance");
            Publication refused;assert(!decode(bad_public.dump(),id,refused));
        }
    }
    assert(public_bands>0);
    std::cout<<"PASS: 3 original forward public publications, "<<public_bands<<" exact bands, instant-page coverage, lifetimes, seeks and provenance refusals\n";
    std::cout<<"PASS: Go-encoded pages retain bytes, anchor coverage, same-time ties, gaps, replay generation, lane/mode, busy retry and timeout refusal\n";
}
