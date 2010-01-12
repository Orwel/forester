/*
 * Copyright (C) 2009 Kamil Dudka <kdudka@redhat.com>
 *
 * This file is part of sl.
 *
 * sl is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * sl is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with sl.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "symheap.hh"
#include "cl_private.hh"

#include <map>
#include <set>
#include <stack>

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>

namespace SymbolicHeap {

typedef std::vector<int>    TSub;
typedef std::set<int>       TSet;

struct Var {
    const struct cl_type    *clt;
    int /* CodeStorage */   cVarUid;
    int /* val */           placedAt;
    int /* val */           value;
    int /* var */           parent;
    TSub                    subVars;

    // TODO
    Var():
        clt(0),
        cVarUid(-1),
        placedAt(VAL_INVALID),
        value(VAL_INVALID),
        parent(VAL_INVALID)
    {
    }
};

enum EValue {
    EV_HEAP = 0,
    // TODO: EV_HEAP_OBJECT_OF_UNKNOWN_TYPE?
    EV_CUSTOM,
    EV_COMPOSITE
};

struct Value {
    EValue                  code;
    const struct cl_type    *clt;
    int /* obj */           pointsTo;
    TSet                    haveValue;

    // TODO
    Value():
        code(EV_HEAP),
        clt(0),
        pointsTo(OBJ_INVALID)
    {
    }
};

typedef std::map<int, int> TIdMap;
typedef std::map<int, Var> TVarMap;
typedef std::map<int, Value> TValueMap;

// /////////////////////////////////////////////////////////////////////////////
// SymHeap implementation
struct SymHeap::Private {
    TIdMap                  cVarIdMap;
    TIdMap                  cValIdMap;
    TVarMap                 varMap;
    TValueMap               valueMap;

    int                     lastObj;
    int                     lastVal;

    // TODO: move elsewhere
    int                     retVal;

    Private();
    void initReturn();

    void releaseValueOf(int obj);
    void indexValueOf(int obj, int val);

    int /* val */ createValue(EValue code, const struct cl_type *clt, int obj);
    int /* var */ createVar(const struct cl_type *clt,
                            int /* CodeStorage */ uid);

    void destroySingleVar(int var);
    void destroyVar(int var);

    void createSubs(int var, const struct cl_type *clt);
};

void SymHeap::Private::initReturn() {
    // create OBJ_RETURN
    Var &var = this->varMap[OBJ_RETURN];

    var.clt         = 0;
    var.cVarUid     = -1;
    var.placedAt    = VAL_INVALID;
    var.value       = VAL_UNINITIALIZED;
}

SymHeap::Private::Private():
    lastObj(0),
    lastVal(0),
    retVal(VAL_INVALID)
{
    this->initReturn();
}

SymHeap::SymHeap():
    d(new Private)
{
}

SymHeap::SymHeap(const SymHeap &ref):
    d(new Private(*ref.d))
{
}

SymHeap::~SymHeap() {
    delete d;
}

SymHeap& SymHeap::operator=(const SymHeap &ref) {
    delete d;
    d = new Private(*ref.d);
    return *this;
}

void SymHeap::Private::releaseValueOf(int obj) {
    // TODO: implement
    const int val = this->varMap[obj].value;
    if (val <= 0)
        return;

    Value &ref = this->valueMap[val];
    if (EV_COMPOSITE == ref.code)
        return;

    TSet &hv = ref.haveValue;
    if (1 != hv.erase(obj))
        TRAP;
}

void SymHeap::Private::indexValueOf(int obj, int val) {
    // TODO: implement
    Value &ref = this->valueMap[val];
    TSet &hv = ref.haveValue;
    hv.insert(obj);
}

int /* val */ SymHeap::Private::createValue(EValue code,
                                            const struct cl_type *clt, int obj)
{
    const int valId = ++lastVal;

    Value &val = valueMap[valId];
    val.code            = code;
    val.clt             = clt;
    val.pointsTo        = obj;

    return valId;
}

int /* var */ SymHeap::Private::createVar(const struct cl_type *clt,
                                          int /* CodeStorage */ uid)
{
    const int objId = ++(this->lastObj);
    Var &var = this->varMap[objId];

    var.clt         = clt;
    var.cVarUid     = uid;
    var.placedAt    = this->createValue(EV_HEAP, clt, objId);
    var.value       = VAL_UNINITIALIZED;

    return objId;
}

void SymHeap::Private::destroySingleVar(int var) {
    TVarMap::iterator varIter = this->varMap.find(var);
    if (this->varMap.end() == varIter)
        // var not found
        TRAP;

    const Var &refVar = varIter->second;
    const bool isHeapVar = (-1 == refVar.cVarUid || !refVar.clt);

    // keep haveValue() up2date
    this->releaseValueOf(var);

    // mark corresponding value as freed
    const int val = refVar.placedAt;
    Value &ref = this->valueMap[val];
    ref.pointsTo = (isHeapVar)
        ? OBJ_DELETED
        : OBJ_LOST;

    this->varMap.erase(varIter);
}

void SymHeap::Private::destroyVar(int var) {
    typedef std::stack<int /* var */> TStack;
    TStack todo;

    // we are using explicit stack to avoid recursion
    todo.push(var);
    while (!todo.empty()) {
        const int var = todo.top();
        todo.pop();

        TVarMap::iterator varIter = this->varMap.find(var);
        if (this->varMap.end() == varIter)
            // var not found
            TRAP;

        // schedule all subvars for removal
        Var &refVar = varIter->second;
        TSub &subs = refVar.subVars;
        BOOST_FOREACH(int subVar, subs) {
            todo.push(subVar);
        }

        // remove current
        this->destroySingleVar(var);
    }
}

void SymHeap::Private::createSubs(int var, const struct cl_type *clt) {
    typedef std::pair<int /* var */, const struct cl_type *> TPair;
    typedef std::stack<TPair> TStack;
    TStack todo;

    // we are using explicit stack to avoid recursion
    push(todo, var, clt);
    while (!todo.empty()) {
        int var;
        const struct cl_type *clt;
        boost::tie(var, clt) = todo.top();
        todo.pop();

        // FIXME: check whether clt may be NULL at this point
        const enum cl_type_e code = clt->code;
        switch (code) {
            case CL_TYPE_CHAR:
                CL_MSG_STREAM_INTERNAL(cl_warn, "warning: "
                        "CL_TYPE_CHAR is not supported by SymHeap for now");
                break;

            case CL_TYPE_BOOL:
            case CL_TYPE_INT:
            case CL_TYPE_PTR:
                break;

            case CL_TYPE_STRUCT: {
                const int cnt = clt->item_cnt;
                Var &ref = this->varMap[var];
                ref.value = this->createValue(EV_COMPOSITE, clt, var);
                ref.subVars.resize(cnt);
                for (int i = 0; i < cnt; ++i) {
                    const struct cl_type *subClt = clt->items[i].type;
                    const int subVar = this->createVar(subClt, -1);
                    ref.subVars[i] = subVar;

                    Var &subRef = /* FIXME: suboptimal */ this->varMap[subVar];
                    subRef.parent = var;
                    push(todo, subVar, subClt);
                }
                break;
            }

            default:
                TRAP;
        }
    }
}

int /* val */ SymHeap::valueOf(int obj) const {
    switch (obj) {
        case OBJ_INVALID:
            return VAL_INVALID;

        case OBJ_LOST:
        case OBJ_DELETED:
        case OBJ_DEREF_FAILED:
            return VAL_DEREF_FAILED;

        case OBJ_UNKNOWN:
            return VAL_UNKNOWN;

        default:
            break;
    }

    TVarMap::iterator iter = d->varMap.find(obj);
    if (d->varMap.end() == iter)
        return VAL_INVALID;

    Var &var = iter->second;
    return var.value;
}

int /* val */ SymHeap::placedAt(int obj) const {
    TVarMap::iterator iter = d->varMap.find(obj);
    if (d->varMap.end() == iter)
        return VAL_INVALID;

    Var &var = iter->second;
    return var.placedAt;
}

int /* obj */ SymHeap::pointsTo(int val) const {
    TValueMap::iterator iter = d->valueMap.find(val);
    if (d->valueMap.end() == iter)
        return OBJ_INVALID;

    Value &value = iter->second;
    if (EV_HEAP != value.code)
        TRAP;

    return value.pointsTo;
}

void SymHeap::haveValue(TCont /* obj[] */ &dst, int val) const {
    TValueMap::iterator iter = d->valueMap.find(val);
    if (d->valueMap.end() == iter)
        return;

    Value &value = iter->second;
    BOOST_FOREACH(int obj, value.haveValue) {
        dst.push_back(obj);
    }
}

void SymHeap::notEqualTo(TCont /* obj[] */ &dst, int obj) const {
    // TODO
    TRAP;
}

bool SymHeap::notEqual(int obj1, int obj2) const {
    // TODO
    TRAP;
    return false;
}

const struct cl_type* /* clt */ SymHeap::objType(int obj) const {
    // first look for Var object
    TVarMap::iterator varIter = d->varMap.find(obj);
    if (d->varMap.end() != varIter) {
        // obj is a Var object
        Var &var = varIter->second;
        return var.clt;
    }

    // then look for Sls object
    // TODO
    TRAP;
    return 0;
}

const struct cl_type* /* clt */ SymHeap::valType(int val) const {
    TValueMap::iterator iter = d->valueMap.find(val);
    if (d->valueMap.end() == iter)
        return 0;

    const Value &ref = iter->second;
    return ref.clt;
}

int /* CodeStorage var uid */ SymHeap::cVar(int var) const {
    TVarMap::iterator iter = d->varMap.find(var);
    if (d->varMap.end() == iter)
        return -1;

    const Var &ref = iter->second;
    return (ref.clt)
        ? ref.cVarUid
        : /* anonymous object of known size */ -1;
}

int /* var */ SymHeap::varByCVar(int /* CodeStorage var */ uid) const {
    TIdMap::iterator iter = d->cVarIdMap.find(uid);
    if (d->cVarIdMap.end() == iter)
        return OBJ_INVALID;
    else
        return iter->second;
}

void SymHeap::gatherCVars(TCont &out) const {
    TIdMap::const_iterator ii;
    for (ii = d->cVarIdMap.begin(); ii != d->cVarIdMap.end(); ++ii)
        out.push_back(ii->first);
}

int /* var */ SymHeap::subVar(int var, int nth) const {
    TVarMap::iterator iter = d->varMap.find(var);
    if (d->varMap.end() == iter)
        return OBJ_INVALID;

    const Var &refVar = iter->second;
    const TSub &subs = refVar.subVars;
    const int cnt = subs.size();
    return (nth < cnt)
        ? subs[nth]
        : OBJ_INVALID;
}

int /* var */ SymHeap::varParent(int var) const {
    TVarMap::iterator iter = d->varMap.find(var);
    if (d->varMap.end() == iter)
        return OBJ_INVALID;

    const Var &refVar = iter->second;
    return refVar.parent;
}

int /* obj */ SymHeap::valGetCompositeObj(int val) const {
    TValueMap::iterator iter = d->valueMap.find(val);
    if (d->valueMap.end() == iter)
        return OBJ_INVALID;

    const Value &ref = iter->second;
    return (EV_COMPOSITE == ref.code)
        ? /* FIXME: use union instead */ ref.pointsTo
        : OBJ_INVALID;
}

int /* var */ SymHeap::varCreate(const struct cl_type *clt,
                                 int /* CodeStorage var */ uid)
{
    const enum cl_type_e code = clt->code;
    switch (code) {
        case CL_TYPE_INT:
            CL_DEBUG("CL_TYPE_INT treated as pointer");
            // go through!

        case CL_TYPE_BOOL:
        case CL_TYPE_PTR:
        case CL_TYPE_STRUCT:
            break;

        default:
            TRAP;
    }

    const int objId = d->createVar(clt, uid);
    d->createSubs(objId, clt);

    if (/* heap object */ -1 != uid)
        d->cVarIdMap[uid] = objId;

    return objId;
}

int /* var */ SymHeap::varCreateAnon(int cbSize) {
    return d->createVar(0, /* FIXME: use union for this? */ cbSize);
}

int SymHeap::varSizeOfAnon(int var) const {
    TVarMap::iterator iter = d->varMap.find(var);
    if (d->varMap.end() == iter)
        // not even a variable
        TRAP;

    Var &ref = iter->second;
    if (ref.clt)
        // not anonoymous variable at all
        TRAP;

    return /* cbSize */ ref.cVarUid;
}

bool SymHeap::valPointsToAnon(int val) const {
    if (val <= 0)
        return false;

    TValueMap::iterator valIter = d->valueMap.find(val);
    if (d->valueMap.end() == valIter)
        // not even a value
        TRAP;

    Value &value = valIter->second;
    if (EV_HEAP != value.code)
        return false;

    TVarMap::iterator iter = d->varMap.find(value.pointsTo);
    if (d->varMap.end() == iter)
        // not even a variable
        TRAP;

    const Var &ref = iter->second;
    return !ref.clt;
}

void SymHeap::varDefineType(int var, const struct cl_type *clt) {
    TVarMap::iterator varIter = d->varMap.find(var);
    if (d->varMap.end() == varIter)
        // var not found
        TRAP;

    Var &refVar = varIter->second;
    if (refVar.clt)
        // type redefinition not allowed
        TRAP;

    refVar.cVarUid = /* heap object */ -1;
    refVar.clt     = clt;
    d->createSubs(var, clt);

    if (OBJ_RETURN == var)
        return;

    TValueMap::iterator valIter = d->valueMap.find(refVar.placedAt);
    if (d->valueMap.end() == valIter)
        TRAP;

    Value &value = valIter->second;
    if (value.clt || EV_HEAP != value.code)
        TRAP;

    value.clt = clt;
}

int /* sls */ SymHeap::slsCreate(const struct cl_type *clt,
                                 const struct cl_accessor *selector)
{
    // TODO
    TRAP;
    return OBJ_INVALID;
}

void SymHeap::objSetValue(int obj, int val) {
    d->releaseValueOf(obj);
    d->indexValueOf(obj, val);

    // first look for Var object
    TVarMap::iterator varIter = d->varMap.find(obj);
    if (d->varMap.end() != varIter) {
        // obj is a Var object
        Var &var = varIter->second;
        var.value = val;
        return;
    }

    // then look for Sls object
    // TODO
    TRAP;
}

void SymHeap::objDestroy(int obj) {
    // first look for Var object
    TVarMap::iterator varIter = d->varMap.find(obj);
    if (d->varMap.end() != varIter) {
        const Var &var = varIter->second;
        const int uid = var.cVarUid;
        if (/* heap object */ -1 != uid)
            d->cVarIdMap.erase(uid);

        d->destroyVar(obj);
        if (OBJ_RETURN == obj)
            d->initReturn();

        return;
    }

    // then look for Sls object
    // TODO
    TRAP;
}

void SymHeap::addNeq(int obj1, int obj2) {
    // TODO
    TRAP;
}

void SymHeap::delNeq(int obj1, int obj2) {
    // TODO
    TRAP;
}

int /* val */ SymHeap::valCreateCustom(const struct cl_type *clt, int cVal)
{
    TIdMap::iterator ii = d->cValIdMap.find(cVal);
    if (d->cValIdMap.end() == ii) {
        const int val = d->createValue(EV_CUSTOM, clt, cVal);
        d->cValIdMap[cVal] = val;
        return val;
    }

    // custom value already defined, we has to reuse it
    const int val = ii->second;
    TValueMap::iterator vi = d->valueMap.find(val);
    if (d->valueMap.end() == vi)
        TRAP;

    const Value &ref = vi->second;
    if (EV_CUSTOM != ref.code)
        // heap corruption
        TRAP;

    if (ref.clt != clt)
        // type mismatch
        TRAP;

    return val;
}

int /* cVal */ SymHeap::valGetCustom(const struct cl_type **pClt, int val) const
{
    TValueMap::iterator iter = d->valueMap.find(val);
    if (d->valueMap.end() == iter)
        // value not found, this should never happen
        TRAP;

    Value &value = iter->second;
    if (EV_CUSTOM != value.code)
        // not a custom value
        return VAL_INVALID;

    if (pClt)
        // TODO: this deserves a comment in the public header
        *pClt = value.clt;

    return /* cVal */ value.pointsTo;
}

} // namespace SymbolicHeap
