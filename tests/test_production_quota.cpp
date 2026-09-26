#include "protodd/ProductionQuota.hpp"
#include <iostream>
int main(){
    protodd::ProductionQuota q;int errors=0;
    auto check=[&](bool ok){if(!ok)++errors;};
    check(!q.wants(0,0));check(q.set(0,{2,4,4,4,4,1,1,4}));check(!q.wants(7,1));
    check(!q.set(0,{4,0,0,0,0,0,0,0}));check(!q.set(24,{4,0,0,0,0,0,0,0}));
    q.result(7,0,true);check(q.wants(8,0));q.result(8,0,true);check(!q.wants(9,0));
    q.result(7,5,false);check(q.wants(8,5));q.result(8,5,false);check(!q.wants(9,5));
    check(!q.wants(240,6));check(q.set(240,{1,0,0,0,0,0,0,0}));check(q.wants(247,0));
    check(!q.wants(7200,0));check(!q.set(7200,{1,0,0,0,0,0,0,0}));
    if(errors)std::cerr<<errors<<" quota checks failed\n";return errors?1:0;
}
