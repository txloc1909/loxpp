#include <iostream>
#include <cassert>

void test_example() {
    assert(1 + 1 == 2);
    std::cout << "Test passed!" << std::endl;
}

int main() {
    test_example();
    std::cout << "All tests completed successfully!" << std::endl;
    return 0;
}
