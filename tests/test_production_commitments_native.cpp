#include "protodd/ProductionCommitments.hpp"
#include <iostream>
int main(){
    using C=protodd::ProductionCommitments;int errors=0;
    const auto check=[&](bool ok,const char* text){if(!ok){++errors;std::cerr<<text<<'\n';}};
    const auto rejects=[&](auto f){try{f();return false;}catch(const std::invalid_argument&){return true;}};
    C c;C::Snapshot s{0,100,0,{1,2}};c.observe(s);
    c.submit({1,0,2,240});check(!c.submit({1,0,2,240}),"duplicate submission");
    auto ids=c.reserve({{0,1,50,0},{0,2,50,0}});check(ids.size()==2&&c.reservations()[0]==100,"double spending");
    check(c.reserve({{0,1,50,0}}).empty(),"duplicate dispatch");
    c.accepted(1);c.accepted(1);check(c.reservations()[0]==100,"acceptance released resources");
    check(rejects([&]{c.rejected(1);}),"accepted relabelled rejected");
    auto bad=s;bad.frame=1;bad.completed={1};check(rejects([&]{c.observe(bad);}),"completion without spending");
    check(c.reservations()[0]==100,"invalid observation mutated state");
    c.rejected(2);s.frame=1;s.minerals=50;s.spent={1};c.observe(s);c.observe(s);
    check(c.reservations()[0]==0,"spend double reserved");
    ids=c.reserve({{0,2,50,0}});check(ids.size()==1&&ids[0]==3,"bounded retry missing");c.rejected(3);
    check(c.reserve({{0,2,50,0}}).empty(),"unbounded retry");
    s.frame=2;s.completed={1};c.observe(s);check(c.ticket(1).phase==C::Phase::completed,"completion missing");
    C death;death.observe({0,100,0,{9}});death.submit({1,1,1,240});death.reserve({{1,9,100,0}});death.accepted(1);
    death.observe({1,100,0,{}});check(death.ticket(1).phase==C::Phase::uncertain&&death.reservations()[0]==100,"death lost uncertain reservation");
    check(rejects([&]{death.rejected(1);}),"accepted death relabelled rejected");
    death.disable();check(death.reserve({{1,9,100,0}}).empty()&&death.reservations()[0]==100,"fallback erased pending work");
    C build;build.observe({0,100,0,{9}});build.submit({1,1,1,2});build.reserve({{1,9,100,0}});build.accepted(1);
    build.observe({1,0,0,{9},{1},{},{},{1}});check(!build.ticket(1).holdsActor()&&build.ticket(1).phase==C::Phase::accepted,"build start conflated with completion");
    build.cancel(1);build.observe({2,100,0,{9}});check(build.reserve({{1,9,100,0}}).empty(),"expired/cancelled retried");
    C producer;producer.observe({0,100,0,{9}});producer.submit({1,0,2,240});producer.reserve({{0,9,50,0}});producer.accepted(1);
    producer.observe({1,50,0,{9},{1},{},{},{1}});
    check(producer.reserve({{0,9,50,0,true,false,true}}).empty(),"busy producer reused");
    check(producer.reserve({{0,9,50,0}}).size()==1,"available queue slot not released after product binding");
    check(producer.ticket(1).phase==C::Phase::accepted,"queue slot release completed product early");
    return errors?1:0;
}
