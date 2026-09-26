#include "protodd/ProductionDemandModel.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

int main(int argc,char** argv){
    try {
        if(argc!=4){std::cerr<<"Usage: production_demand_cpu WEIGHTS INPUT OUTPUT\n";return 2;}
        protodd::ProductionDemandModel model;std::string error;
        std::ifstream weights(argv[1],std::ios::binary);
        if(!model.load(weights,error))throw std::runtime_error(error);
        std::ifstream input(argv[2],std::ios::binary);
        std::uint32_t rows{},columns{};
        if(!input.read(reinterpret_cast<char*>(&rows),4)||!input.read(reinterpret_cast<char*>(&columns),4)||
            rows==0||rows>1000000||columns!=model.inputCount())throw std::runtime_error("invalid input dimensions");
        if(std::filesystem::exists(argv[3]))throw std::runtime_error("output already exists");
        std::ofstream output(argv[3],std::ios::binary);
        if(!output)throw std::runtime_error("cannot create output");
        std::vector<float> features(columns);std::vector<double> timings;timings.reserve(rows);
        for(std::uint32_t row=0;row<rows;++row){
            if(!input.read(reinterpret_cast<char*>(features.data()),columns*4))throw std::runtime_error("truncated features");
            const auto start=std::chrono::steady_clock::now();
            const auto result=model.predict(features);
            const auto micros=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
            if(!result)throw std::runtime_error("invalid model input or output");
            timings.push_back(micros);
            output.write(reinterpret_cast<const char*>(result->ordinalLogits.data()),32*4);
            for(const auto count:result->quantities){const float f=static_cast<float>(count);output.write(reinterpret_cast<const char*>(&f),4);}
        }
        if(input.peek()!=std::char_traits<char>::eof())throw std::runtime_error("trailing input data");
        if(!output)throw std::runtime_error("output write failed");
        // Invalid input and failed load must fail closed without destroying a
        // previously verified model. These checks never send a game command.
        features[0]=std::numeric_limits<float>::quiet_NaN();
        if(model.predict(features))throw std::runtime_error("nonfinite input accepted");
        std::istringstream invalid("invalid");
        if(model.load(invalid,error)||model.inputCount()!=columns)throw std::runtime_error("invalid reload handling");
        std::ranges::sort(timings);
        std::cout<<"{\"rows\":"<<rows<<",\"pointer_bits\":"<<sizeof(void*)*8
            <<",\"p50_us\":"<<timings[rows/2]<<",\"p95_us\":"<<timings[static_cast<std::size_t>(rows*.95)]
            <<",\"max_us\":"<<timings.back()<<",\"invalid_input_checks\":true}\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
