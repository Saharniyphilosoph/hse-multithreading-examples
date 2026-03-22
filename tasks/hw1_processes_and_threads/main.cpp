#include "apply_function.h"

#include <iostream>
#include <vector>

int main() {
    std::vector<int> data{8, 10, 11, 3, 7};

    ApplyFunction<int>(data, [](int& value) { value *= 2; }, 3);

    for (const int value : data) {
        std::cout << value << ' ';
    }
    std::cout << '\n';

    return 0;
}
