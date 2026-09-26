#include "protodd/ProductionHistory.hpp"
#include <cmath>
#include <iostream>
int main(){
    using namespace protodd;int errors=0;
    const auto check=[&](bool ok,const char* msg){if(!ok){++errors;std::cerr<<msg<<'\n';}};
    ProductionHistory h;
    check(productionBuildCenter(112,15,2,2)==Position{3616,512},"pylon center mismatch");
    check(productionBuildCenter(117,17,4,3)==Position{3808,592},"gateway half-tile center mismatch");
    auto x=h.sample(0);check(x[0]==0&&x[8]==1,"empty history");
    h.accepted(0,0);x=h.sample(0);check(x[0]==0&&x[8]==1,"same-frame leaked");
    x=h.sample(24);check(x[0]==.25F&&std::abs(x[8]-.01F)<1e-7F,"past event missing");
    for(int i=0;i<6;++i)h.accepted(24,0);
    h.accepted(24,7);x=h.sample(24);check(x[0]==.25F&&x[7]==0,"sample not immutable");
    x=h.sample(240);check(x[0]==1&&x[7]==.25F,"counts not clipped");
    x=h.sample(264);check(x[0]==1&&x[7]==.25F,"left boundary not inclusive");
    x=h.sample(265);check(x[0]==0&&x[7]==0,"window end wrong");
    x=h.sample(2425);check(x[8]==1&&x[15]==1,"old age not capped");
    bool threw=false;try{h.accepted(20,0);}catch(...){threw=true;}check(threw,"backdating allowed");
    h.reset();x=h.sample(0);check(x[8]==1,"reset leaked prior game");
    return errors?1:0;
}
