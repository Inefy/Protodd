#include "protodd/PolicyLearning.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3 || std::filesystem::exists(argv[2])) {
        std::cerr << "usage: policy_trainer validated-transitions.txt NEW-Policy.q\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) return 2;
    protodd::PolicyLearner learner;
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string context, extra;
        int state, action, next, mask, terminal;
        double reward;
        if (!(row >> context >> state >> action >> reward >> next >> mask >> terminal) ||
            (row >> extra) || state < 0 || state >= 256 || action < 0 || action >= 4 ||
            next < 0 || next >= 256 || mask < 0 || mask > 15 || terminal < 0 || terminal > 1 ||
            !learner.update(context, state, static_cast<protodd::PolicyAction>(action),
                            reward, next, static_cast<protodd::PolicyActionMask>(mask), terminal != 0)) {
            std::cerr << "Invalid transition: " << count + 1 << '\n';
            return 3;
        }
        ++count;
    }
    if (!count) return 3;
    std::ofstream output(argv[2], std::ios::binary);
    output << learner.serialize();
    if (!output) return 4;
    std::cout << "Trained " << count << " transitions, " << learner.stateCount() << " states\n";
}
