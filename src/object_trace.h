#pragma once

// What a collector needs to know about each object type: the references that
// an object holds (traceObjectEdges) and the size that create<T> counted for
// it (objAllocatedSize). The runtime owns this knowledge, and every collector
// uses it. docs/decisions/garbage-collectors.md, section 5.

#include "objects.h"
#include "value.h"

#include <cstddef>
#include <cstdio>

// Calls m.markObject and m.markValue for each reference that `obj` holds.
// A type that holds none has its own case, so a new type must be added here.
template <typename Marker>
inline void traceObjectEdges(Obj* obj, Marker& m) {
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] trace  %p %s\n", static_cast<void*>(obj),
            stringifyObj(obj).c_str());
#endif
    switch (obj->type) {
    case ObjType::STRING:
        break;
    case ObjType::NATIVE:
        break;
    case ObjType::UPVALUE:
        m.markValue(static_cast<ObjUpvalue*>(obj)->closed);
        m.markObject(static_cast<ObjUpvalue*>(obj)->owner);
        break;
    case ObjType::FUNCTION: {
        auto* fn = static_cast<ObjFunction*>(obj);
        m.markObject(fn->name);
        const auto& consts = fn->chunk.constants();
        for (uint16_t i = 0; i < consts.size(); i++) {
            m.markValue(consts.at(i));
        }
        break;
    }
    case ObjType::CLOSURE: {
        auto* cl = static_cast<ObjClosure*>(obj);
        m.markObject(cl->function);
        for (auto* uv : cl->upvalues) {
            m.markObject(uv);
        }
        break;
    }
    case ObjType::CLASS: {
        auto* klass = static_cast<ObjClass*>(obj);
        m.markObject(klass->name);
        if (klass->superclass) {
            m.markObject(klass->superclass);
        }
        klass->methods.forEach([&m](ObjString* k, Value v) {
            m.markObject(k);
            m.markValue(v);
        });
        break;
    }
    case ObjType::INSTANCE: {
        auto* inst = static_cast<ObjInstance*>(obj);
        m.markObject(inst->klass);
        inst->fields.forEach([&m](ObjString* k, Value v) {
            m.markObject(k);
            m.markValue(v);
        });
        break;
    }
    case ObjType::BOUND_METHOD: {
        auto* bm = static_cast<ObjBoundMethod*>(obj);
        m.markValue(bm->receiver);
        m.markObject(bm->method);
        break;
    }
    case ObjType::LIST: {
        auto* list = static_cast<ObjList*>(obj);
        for (auto& v : list->elements) {
            m.markValue(v);
        }
        break;
    }
    case ObjType::FILE:
        m.markObject(static_cast<ObjFile*>(obj)->klass);
        break;
    case ObjType::ITERATOR: {
        auto* it = static_cast<ObjIterator*>(obj);
        m.markValue(it->collection);
        if (it->hasCurrent) {
            m.markValue(it->current);
        }
        break;
    }
    case ObjType::MAP: {
        auto* map = static_cast<ObjMap*>(obj);
        m.markObject(map->klass);
        map->map.forEach([&m](const MapEntry& e) {
            m.markValue(e.key);
            m.markValue(e.value);
        });
        break;
    }
    case ObjType::ENUM_CTOR: {
        auto* ctor = static_cast<ObjEnumCtor*>(obj);
        m.markObject(ctor->ctorName);
        m.markObject(ctor->enumName);
        break;
    }
    case ObjType::ENUM: {
        auto* e = static_cast<ObjEnum*>(obj);
        m.markObject(e->ctor);
        for (auto& v : e->fields) {
            m.markValue(v);
        }
        break;
    }
    case ObjType::BOUND_NATIVE: {
        auto* bn = static_cast<ObjBoundNative*>(obj);
        m.markValue(bn->receiver);
        m.markObject(bn->native);
        break;
    }
    case ObjType::ERROR: {
        auto* err = static_cast<ObjError*>(obj);
        m.markObject(err->klass);
        m.markObject(err->message);
        m.markObject(err->kind);
        break;
    }
    case ObjType::DEFERRED_CALL: {
        auto* deferred = static_cast<ObjDeferredCall*>(obj);
        m.markValue(deferred->callable);
        for (auto& v : deferred->args) {
            m.markValue(v);
        }
        break;
    }
    case ObjType::SOCKET: {
        auto* sock = static_cast<ObjSocket*>(obj);
        m.markObject(sock->klass);
        m.markObject(sock->readHolder);
        m.markObject(sock->writeHolder);
        break;
    }
    case ObjType::SERVER: {
        auto* srv = static_cast<ObjServer*>(obj);
        m.markObject(srv->klass);
        m.markObject(srv->acceptHolder);
        break;
    }
    case ObjType::PROCESS:
        m.markObject(static_cast<ObjProcess*>(obj)->klass);
        break;
    case ObjType::COROUTINE: {
        auto* co = static_cast<ObjCoroutine*>(obj);
        m.markObject(co->klass);
        m.markValue(co->callee);
        for (const auto& v : co->stack) {
            m.markValue(v);
        }
        for (const auto& frame : co->frames) {
            m.markObject(frame.closure);
        }
        for (const auto& deferList : co->defers) {
            for (const auto& v : deferList) {
                m.markValue(v);
            }
        }
        for (ObjUpvalue* uv : co->openUpvalues) {
            m.markObject(uv);
        }
        for (const auto& v : co->resultOverrides) {
            m.markValue(v);
        }
        if (co->waitingInner != nullptr) {
            m.markObject(co->waitingInner);
        }
        if (co->waitingIterator != nullptr) {
            m.markObject(co->waitingIterator);
        }
        break;
    }
    case ObjType::TASK: {
        auto* task = static_cast<ObjTask*>(obj);
        if (task->co != nullptr) {
            m.markObject(task->co);
        }
        for (const auto& v : task->mailbox) {
            m.markValue(v);
        }
        m.markValue(task->startArg);
        m.markValue(task->pendingValue);
        m.markValue(task->sel.want);
        if (task->io.target != nullptr) {
            m.markObject(task->io.target);
        }
        if (task->io.text != nullptr) {
            m.markObject(task->io.text);
        }
        break;
    }
    case ObjType::INT:
        // An Int holds no reference. Missing this case would leave the box
        // unmarked and free it under a live constant, so the probe that keeps
        // a heap-held Int in a List across many collections pins it.
        break;
    }
}

// Returns the sizeof(T) used when the object was created via create<T>().
// Must match the sizeof(T) added in create<T>().
inline std::size_t objAllocatedSize(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING:
        return sizeof(ObjString);
    case ObjType::FUNCTION:
        return sizeof(ObjFunction);
    case ObjType::NATIVE:
        return sizeof(ObjNative);
    case ObjType::UPVALUE:
        return sizeof(ObjUpvalue);
    case ObjType::CLOSURE:
        return sizeof(ObjClosure);
    case ObjType::CLASS:
        return sizeof(ObjClass);
    case ObjType::INSTANCE:
        return sizeof(ObjInstance);
    case ObjType::BOUND_METHOD:
        return sizeof(ObjBoundMethod);
    case ObjType::LIST:
        return sizeof(ObjList);
    case ObjType::FILE:
        return sizeof(ObjFile);
    case ObjType::ITERATOR:
        return sizeof(ObjIterator);
    case ObjType::MAP:
        return sizeof(ObjMap);
    case ObjType::ENUM_CTOR:
        return sizeof(ObjEnumCtor);
    case ObjType::ENUM:
        return sizeof(ObjEnum);
    case ObjType::BOUND_NATIVE:
        return sizeof(ObjBoundNative);
    case ObjType::ERROR:
        return sizeof(ObjError);
    case ObjType::DEFERRED_CALL:
        return sizeof(ObjDeferredCall);
    case ObjType::SOCKET:
        return sizeof(ObjSocket);
    case ObjType::SERVER:
        return sizeof(ObjServer);
    case ObjType::PROCESS:
        return sizeof(ObjProcess);
    case ObjType::COROUTINE:
        return sizeof(ObjCoroutine);
    case ObjType::TASK:
        return sizeof(ObjTask);
    case ObjType::INT:
        return sizeof(ObjInt);
    }
    return 0;
}
