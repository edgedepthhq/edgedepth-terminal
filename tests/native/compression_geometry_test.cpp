#include "core/compression_geometry.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "%s\n",message); std::exit(1); } }
int main() {
    compression::Geometry q{1790064000000,1764504000000,1788926400000,1764504000000,1770336000000,1787054400000,108,-1.5608621593291406e-09,53.27,1.2979710977126965e-10};
    require(q.valid(),"QNT fixture accepted");
    require(!q.visible(q.asof-1),"No future geometry before decision time");
    require(q.visible(q.asof),"Geometry becomes visible at its decision time");
    require(std::abs(q.resistance(1790072100000.0)*1.005-68.43217882517688)<1e-7,"QNT time-projected minute trigger");
    const double before=q.resistance(static_cast<double>(q.asof)); (void)q.resistance(static_cast<double>(q.asof+86400000));
    require(q.resistance(static_cast<double>(q.asof))==before,"Projection never mutates frozen anchors");
    q.touch=q.asof+1; require(!q.valid(),"Reject an unconfirmed future anchor");
    q.touch=1788926400000; q.slope=NAN; require(!q.valid(),"Reject missing geometry");
}
