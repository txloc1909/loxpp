#pragma once

#include <cstdint>
#include "value.h"

struct ObjString;

struct Entry {
    ObjString* key{nullptr};
    Value value;
};

struct Table {
    int count{0};
    int capacity{0};
    Entry* entries{nullptr};
};

void initTable(Table* table);
void freeTable(Table* table);

bool tableSet(Table* table, ObjString* key, Value value);
bool tableGet(Table* table, ObjString* key, Value& value);
bool tableDelete(Table* table, ObjString* key);
void tableAddAll(Table* from, Table* to);

uint32_t hashString(std::string_view s);

ObjString* tableFindString(Table* table, const char* chars, int length,
                           uint32_t hash);
