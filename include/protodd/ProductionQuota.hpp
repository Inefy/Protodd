#pragma once
#include <array>
#include <stdexcept>
namespace protodd {
// A forecast covers one fixed 240-frame interval. Repeated 24-frame inference
// must not turn the same predicted unit into ten independent requests.
class ProductionQuota {
public:
    bool set(int observation,const std::array<int,8>& quantities) {
        if(observation<0||observation%240||observation>=7200||observation<=sample_)return false;
        for(int q:quantities)if(q<0||q>4)throw std::invalid_argument("invalid production quantity");
        sample_=observation;remaining_={};attempts_={};
        for(int a:{0,5,6})remaining_[a]=quantities[a];return true;
    }
    bool wants(int frame,int action) const {
        return action>=0&&action<8&&sample_>=0&&frame>=sample_&&frame<sample_+240&&frame<7200&&
            remaining_[action]>0&&attempts_[action]<2;
    }
    void result(int frame,int action,bool accepted) {
        if(!wants(frame,action))throw std::invalid_argument("dispatch outside quota");
        if(accepted){--remaining_[action];attempts_[action]=0;}else ++attempts_[action];
    }
private:
    int sample_{-1};std::array<int,8> remaining_{},attempts_{};
};
}
