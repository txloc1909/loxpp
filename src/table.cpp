#include "table.h"
#include "object.h"

void initTable(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = nullptr;
}

void freeTable(Table* table) {
    delete[] table->entries;
    initTable(table);
}

bool tableSet(Table* /*table*/, ObjString* /*key*/, Value /*value*/) {
    return false; // TODO
}

bool tableGet(Table* /*table*/, ObjString* /*key*/, Value& /*value*/) {
    return false; // TODO
}

bool tableDelete(Table* /*table*/, ObjString* /*key*/) {
    return false; // TODO
}

void tableAddAll(Table* /*from*/, Table* /*to*/) {
    // TODO
}

ObjString* tableFindString(Table* /*table*/, const char* /*chars*/,
                           int /*length*/, uint32_t /*hash*/) {
    return nullptr; // TODO
}
