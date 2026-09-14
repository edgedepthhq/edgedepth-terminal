#include "core/workspace_document.h"
#include <cstdio>
#include <cstdlib>
using workspace::Json;
static void check(bool ok) { if (!ok) { std::fputs("workspace check failed\n",stderr); std::exit(1); } }
int main() {
    for (const std::string source : {"chart_binancef_btcusdt", "###chart_binancef_btcusdt",
                                    "Chart BTC 1m###chart_binancef_btcusdt"}) {
        std::string ini = "[Window][" + source + "]\nDockId=0x00000003,0\n\n[Window][Watchlist]\nDockId=0x00000001\n";
        workspace::remap_window_title(ini, "Chart BTC 1m###chart_binancef_btcusdt",
                                     "Chart 牛来 5m###chart_binancef_牛来usdt");
        if (ini != "[Window][chart_binancef_牛来usdt]\nDockId=0x00000003,0\n\n[Window][Watchlist]\nDockId=0x00000001\n") {
            std::fprintf(stderr, "FAIL symbol-switch docking remap\n");
            return 1;
        }
    }

    Json doc = {{"version",1},{"layout","[Window][Chart]\nPos=0,0\n"},
                {"widgets",Json::array({{{"type","chart"},{"title","Chart"},{"settings",Json::object()}}})}};
    check(workspace::parse_document(doc.dump()) == doc);
    auto bad=doc; bad["version"]=2; check(workspace::parse_document(bad.dump()).is_null());
    bad=doc; bad["widgets"].push_back(bad["widgets"][0]); check(!workspace::valid_document(bad));
    bad=doc; bad["widgets"][0]["type"]="execute"; check(!workspace::valid_document(bad));
    check(workspace::parse_document(std::string(300000,'x')).is_null());
    check(workspace::parse_document(std::string(10000,'[')).is_null());
    check(workspace::parse_document("{broken").is_null());
    int n=3; bool b=true; double x=1;
    workspace::read(Json{{"n",1e100}},"n",n,0,10); check(n==3);
    workspace::read(Json{{"n",2.5}},"n",n,0,10); check(n==3);
    workspace::read(Json{{"n","4"}},"n",n,0,10); check(n==3);
    workspace::read(Json{{"b",0}},"b",b); check(b);
    workspace::read(Json{{"x",2.0}},"x",x,0,10); check(x==2);
    std::puts("workspace document validation passed");
}
