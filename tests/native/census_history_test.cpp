#include "core/census_history.h"
#include <vector>
#include <string>
#include <limits>
#include <cstdio>
#include <cstdlib>
struct Band {double price_mid=100,est_long_usd=1,est_short_usd=0;};
struct CensusQuality {
        uint32_t version = 0;
        int64_t venue_received_at_ms = 0;
        double weighted_wallet_age_ms = 0;
        int64_t p95_wallet_age_ms = 0;
        uint32_t sampled_positions = 0;
        uint32_t usable_positions = 0;
        uint32_t unlocated_positions = 0;
        uint32_t stale_positions = 0;
        double sampled_notional_usd = 0;
        double usable_notional_usd = 0;
        double unlocated_notional_usd = 0;
        double stale_notional_usd = 0;
        double coverage_denominator_usd = 0;
        uint32_t far_filtered_positions = 0;
        double far_filtered_notional_usd = 0;
    };
struct Frame {CensusQuality census_quality;int64_t timestamp_ms=1000,census_observed_at_ms=0;double mark_price=100,flow_intensity=.1,band_width_pct=.25;std::string census_status="sampled";std::vector<Band> bands{{}};};
void check(bool ok){if(!ok){std::fputs("census history invariant failed\n",stderr);std::exit(1);}}
int main(){
 census_history::History<Frame> qh; Frame qf;
 qf.timestamp_ms=400000; qf.census_observed_at_ms=390000;
 qf.census_quality.version=1; qf.census_quality.venue_received_at_ms=399000;
 qf.census_quality.sampled_positions=1; qf.census_quality.usable_positions=1;
 qf.census_quality.sampled_notional_usd=10; qf.census_quality.usable_notional_usd=10;
 qf.census_quality.coverage_denominator_usd=100; check(qh.insert(qf));
 qf.census_quality.venue_received_at_ms=400001;check(!qh.insert(qf));qf.census_quality.venue_received_at_ms=399000;
 qf.census_quality.usable_notional_usd=11;check(!qh.insert(qf));qf.census_quality.usable_notional_usd=10;
 qf.census_quality.coverage_denominator_usd=0;check(!qh.insert(qf));qf.flow_intensity=0;check(qh.insert(qf));
 qf.census_quality.version=0;check(qh.insert(qf));check(qh.at(400000)->census_quality.version==1);
 census_history::History<Frame> h;Frame f;check(h.insert(f));
 check(!h.at(999));check(h.at(1000)->timestamp_ms==1000);
 f.timestamp_ms=100000;check(h.insert(f));check(h.at(50000)->timestamp_ms==1000);
 check(h.display_end(1000,50000)==50000); // Future snapshot cannot shorten the current held rail.
 check(h.display_end(1000,150000)==61000); // Earlier unknown gap remains empty.
 check(h.display_end(100000,150000)==150000); // Latest known report holds to as-of.
 check(h.display_end(100000,50000)==100000); // Future snapshot is not renderable.
 f.timestamp_ms=1000;f.census_status="legacy";check(h.insert(f));check(h.at(1000)->census_status=="sampled");
 f.timestamp_ms=200000;f.census_status="unavailable";f.bands.clear();check(h.insert(f));check(h.at(200000)->census_status=="unavailable");
 f.census_status="invented";check(!h.insert(f));f.census_status="unavailable";
 f.census_observed_at_ms=f.timestamp_ms+1;check(!h.insert(f));f.census_observed_at_ms=0;
 f.band_width_pct=0;check(!h.insert(f));f.band_width_pct=.25;
 f.mark_price=std::numeric_limits<double>::quiet_NaN();check(!h.insert(f));f.mark_price=100;
 for(int64_t i=0;i<1000;++i){f.timestamp_ms=300000+i;check(h.insert(f));}
 check(h.frames().size()==census_history::kMaxFrames);check(!h.at(1000));
}
