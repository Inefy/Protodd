#include "protodd/ProductionDemandModel.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace protodd {
namespace {
bool read32(std::istream& input, std::uint32_t& value) {
    unsigned char data[4]{};
    if (!input.read(reinterpret_cast<char*>(data),4)) return false;
    value=0;
    for (unsigned i=0;i<4;++i) value |= static_cast<std::uint32_t>(data[i])<<(8*i);
    return true;
}
}

bool ProductionDemandModel::load(std::istream& input, std::string& error) {
    const auto fail=[&error](const char* message){error=message;return false;};
    error.clear();
    char magic[8]{};std::uint32_t version{},low{},high{},features{},width{},actions{},levels{};
    if (!input.read(magic,8) || std::string_view(magic,8)!="PTDDEM1\n") return fail("invalid demand magic");
    if (!read32(input,version)||!read32(input,low)||!read32(input,high)||!read32(input,features)||
        !read32(input,width)||!read32(input,actions)||!read32(input,levels)) return fail("truncated demand header");
    const auto fingerprint=static_cast<std::uint64_t>(low)|(static_cast<std::uint64_t>(high)<<32);
    if (version!=1 || fingerprint!=macroSchemaFingerprint() || features!=modelFeatures().size()+16 ||
        features>1024 || width<1 || width>256 || actions!=8 || levels!=4) return fail("demand schema mismatch");
    const std::size_t count=2*features+width*(features+1ULL)+width*(width+1ULL)+32*(width+1ULL);
    std::vector<float> values(count);
    for (std::size_t i=0;i<count;++i) {
        std::uint32_t bits{};if(!read32(input,bits))return fail("truncated demand parameters");
        values[i]=std::bit_cast<float>(bits);
        if (!std::isfinite(values[i]) || std::abs(values[i])>1000.0F) return fail("invalid demand parameter");
        if (i>=features && i<2*features && values[i]<0.01999F) return fail("invalid normalization scale");
    }
    if (input.peek()!=std::char_traits<char>::eof() || input.bad())return fail("trailing demand data");
    inputs_=features;width_=width;parameters_=std::move(values);return true;
}

std::optional<ProductionDemandPrediction> ProductionDemandModel::predict(std::span<const float> features) const noexcept {
    if(parameters_.empty() || features.size()!=inputs_)return std::nullopt;
    std::array<float,1024> normalized{};
    for(std::size_t i=0;i<inputs_;++i){
        if(!std::isfinite(features[i])||features[i]<0||features[i]>16 || (i>=inputs_-16 && features[i]>1))return std::nullopt;
        normalized[i]=(features[i]-parameters_[i])/parameters_[inputs_+i];
    }
    std::array<float,256> first{},second{};
    std::array<float,32> raw{};std::size_t offset=2*inputs_;
    const auto layer=[this,&offset](std::span<const float> source,std::span<float> target,bool relu){
        const auto bias=offset+source.size()*target.size();
        for(std::size_t out=0;out<target.size();++out){
            float sum=parameters_[bias+out];
            for(std::size_t in=0;in<source.size();++in)sum+=source[in]*parameters_[offset+out*source.size()+in];
            target[out]=relu?std::max(0.0F,sum):sum;
        }
        offset=bias+target.size();
    };
    layer(std::span<const float>(normalized.data(),inputs_),std::span<float>(first.data(),width_),true);
    layer(std::span<const float>(first.data(),width_),std::span<float>(second.data(),width_),true);
    layer(std::span<const float>(second.data(),width_),raw,false);
    ProductionDemandPrediction result;
    for(std::size_t action=0;action<8;++action){
        const float base=raw[action*4];float gaps=0;
        for(std::size_t level=0;level<4;++level){
            if(level){const float x=raw[action*4+level];gaps+=std::max(0.0F,x)+std::log1p(std::exp(-std::abs(x)));}
            const float value=base-gaps;
            if(!std::isfinite(value))return std::nullopt;
            result.ordinalLogits[action*4+level]=value;
            result.quantities[action]+=value>=0;
        }
    }
    return result;
}
} // namespace protodd
