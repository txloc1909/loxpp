#pragma once

#include <vector>

using Value = double;

class ValueArray : std::vector<Value> {
  public:
    using std::vector<Value>::size;
    using std::vector<Value>::at;
    void write(Value value);
};
