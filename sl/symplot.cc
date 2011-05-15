/*
 * Copyright (C) 2010 Kamil Dudka <kdudka@redhat.com>
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "symplot.hh"

#include <cl/cl_msg.hh>
#include <cl/storage.hh>
#include <cl/clutil.hh>

#include "symbt.hh"
#include "symheap.hh"
#include "symseg.hh"
#include "symutil.hh"
#include "util.hh"
#include "worklist.hh"

#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stack>
#include <string>

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>

#ifndef DEBUG_SYMPLOT
#   define DEBUG_SYMPLOT 0
#endif

#ifndef SYMPLOT_STOP_AFTER_N_STATES
#   define SYMPLOT_STOP_AFTER_N_STATES 0
#endif

// singleton
class PlotEnumerator {
    public:
        static PlotEnumerator* instance() {
            return (inst_)
                ? (inst_)
                : (inst_ = new PlotEnumerator);
        }

        // generate kind of more unique name
        std::string decorate(std::string name);

    private:
        static PlotEnumerator *inst_;
        PlotEnumerator() { }
        // FIXME: should we care about the destruction?

    private:
        typedef std::map<std::string, int> TMap;
        TMap map_;
};

// /////////////////////////////////////////////////////////////////////////////
// implementation of PlotEnumerator
PlotEnumerator *PlotEnumerator::inst_ = 0;

std::string PlotEnumerator::decorate(std::string name) {
    // obtain a unique ID for the given name
    const int id = map_[name] ++;
#if SYMPLOT_STOP_AFTER_N_STATES
    if (SYMPLOT_STOP_AFTER_N_STATES < id) {
        CL_ERROR("SYMPLOT_STOP_AFTER_N_STATES (" << SYMPLOT_STOP_AFTER_N_STATES
                << ") exceeded, now stopping per user's request...");
        CL_TRAP;
    }
#endif

    // convert the ID to string
    std::ostringstream str;
    str << std::fixed
        << std::setfill('0')
        << std::setw(/* width of the ID suffix */ 4)
        << id;

    // merge name with ID
    name += "-";
    name += str.str();

#ifdef SYMPLOT_STOP_CONDITION
    if (SYMPLOT_STOP_CONDITION(name))
        CL_TRAP;
#endif

    return name;
}


// /////////////////////////////////////////////////////////////////////////////
// implementation of SymPlot
struct SymPlot::Private {
    const CodeStorage::Storage          *stor;
    const SymHeap                       *sh;
    std::ofstream                       dotStream;
    bool                                ok;
    const struct cl_loc                 *lw;
    WorkList<TValId>                  workList;
    std::set<TObjId>                    objDone;
    int                                 last;

    typedef std::pair<TObjId, TValId>                       TEdgeValueOf;
    std::vector<TEdgeValueOf>           evList;

#if 0
    typedef std::pair<TValId, SymHeap::TOffVal>             TEdgeOffVal;
    std::vector<TEdgeOffVal>            ovList;
#endif

    typedef std::pair<TValId, TValId>                       TEdgeNeq;
    std::set<TEdgeNeq>                  neqSet;

    std::set<TObjId>                    heads;
    std::set<TObjId>                    nexts;
    std::set<TObjId>                    peers;

    bool openDotFile(const std::string &name);
    void closeDotFile();

    bool digFieldName(std::string &dst, TObjId obj);
    void plotNodeObj(TObjId obj);
    void plotNodeObjAnon(TObjId obj);
    void plotNodeValue(TValId val, enum cl_type_e code, const char *label);
    void plotNodeAux(int src, enum cl_type_e code, const char *label);
    void plotNeqZero(TValId val);

    void plotEdgePointsTo(TValId value, TObjId obj);
    void plotEdgeValueOf(TObjId obj, TValId value);
    void plotEdgeSub(TObjId obj, TObjId sub);

    void gobbleEdgeValueOf(TObjId obj, TValId value);
    // TODO: void gobbleEdgeOffValue(TValId val, const SymHeap::TOffVal &ov);
    void gobbleEdgeNeq(TValId val1, TValId val2);
    void gobbleEdgeAlias(TValId val1, TValId val2);
    void emitPendingEdges();
    void traverseNeqs(TValId value);

    void plotSingleValue(TValId value);
    void plotZeroValue(TObjId obj);
    void digNext(TObjId obj);
    void openCluster(TObjId obj);

    bool handleCustomValue(TValId value);
    bool handleUnknownValue(TValId value);
    bool resolveValueOf(TValId *pDst, TObjId obj);
    bool resolvePointsTo(TObjId *pDst, TValId val);

    void digObjCore(TObjId obj);
    void digObj(TObjId obj);
    void digValues();
    void plotObj(TObjId obj);
    void plotCVar(CVar cVar);
};

#define SL_QUOTE(what) "\"" << what << "\""

bool SymPlot::Private::openDotFile(const std::string &plotName) {
    // compute a sort of unique file name
    PlotEnumerator *pe = PlotEnumerator::instance();
    std::string name(pe->decorate(plotName));
    std::string fileName(name + ".dot");

    // now please create the file
    this->dotStream.open(fileName.c_str(), std::ios::out);
    if (!this->dotStream) {
        CL_ERROR("unable to create file '" << fileName << "'");
        return false;
    }

    // open graph
    this->dotStream << "digraph " << SL_QUOTE(name) << " {" << std::endl
        << "\tlabel=<<FONT POINT-SIZE=\"18\">"
        << name << "</FONT>>;"                      << std::endl
        << "\tclusterrank=local;"                   << std::endl
        << "\tlabelloc=t;"                          << std::endl;

    CL_DEBUG("symplot: created dot file '" << fileName << "'");
    return this->dotStream;
}

void SymPlot::Private::closeDotFile() {
    // emit pending edges
    this->emitPendingEdges();

    // close graph
    this->dotStream << "}" << std::endl;
    if (!this->dotStream)
        this->ok = false;

    // close stream
    this->dotStream.close();
}

namespace {
    const char* prefixByCode(enum cl_type_e code) {
        switch (code) {
            case CL_TYPE_VOID:      return "void";
            case CL_TYPE_UNKNOWN:   return "?";
            case CL_TYPE_PTR:       return "*";
            case CL_TYPE_FNC:       return "fnc*";
            case CL_TYPE_STRUCT:    return "struct";
            case CL_TYPE_UNION:     return "union";
            case CL_TYPE_ARRAY:     return "array";
            case CL_TYPE_STRING:    return "string";
            case CL_TYPE_CHAR:      return "char";
            case CL_TYPE_BOOL:      return "bool";
            case CL_TYPE_INT:       return "int";
            case CL_TYPE_ENUM:      return "enum";
            default:                return "XXX";
        }
    }

    const char* colorByCode(enum cl_type_e code) {
        switch (code) {
            case CL_TYPE_VOID:      return "red";
            case CL_TYPE_UNKNOWN:   return "gray";
            case CL_TYPE_PTR:       return "blue";
            case CL_TYPE_FNC:       return "green";
            case CL_TYPE_STRUCT:    return "black";
            case CL_TYPE_UNION:     return "red";
            case CL_TYPE_ARRAY:     return "gray";
            case CL_TYPE_STRING:    return "gray";
            case CL_TYPE_CHAR:      return "gray";
            case CL_TYPE_BOOL:      return "gold";
            case CL_TYPE_INT:       return "gray";
            case CL_TYPE_ENUM:      return "gray";
            default:                return "black";
        }
    }
}

bool SymPlot::Private::digFieldName(std::string &dst, TObjId obj) {
    int nth;
    const TObjId parent = this->sh->objParent(obj, &nth);
    if (OBJ_INVALID == parent)
        // no chance since there is no parent
        return false;

    const struct cl_type *clt = this->sh->objType(parent);
    CL_BREAK_IF(!clt || !isComposite(clt));

    const char *name = clt->items[nth].name;
    if (!name)
        // anonymous unions involved?
        return false;

    dst = name;
    return true;
}

void SymPlot::Private::plotNodeObj(TObjId obj) {
    CL_BREAK_IF(obj <= 0);
    const struct cl_type *clt = this->sh->objType(obj);
    const enum cl_type_e code = (clt)
        ? clt->code
        : CL_TYPE_UNKNOWN;

    this->dotStream << "\t" << SL_QUOTE(obj);
    this->dotStream << " [shape=box";

    if (hasKey(this->heads, obj))
        this->dotStream << ", color=green, penwidth=3.0, style=dashed";
    else if (hasKey(this->peers, obj))
        this->dotStream << ", color=gold, penwidth=3.0, style=dashed";
    else if (hasKey(this->nexts, obj))
        this->dotStream << ", color=red, penwidth=3.0, style=dashed";
    else
        this->dotStream << ", color=" << colorByCode(code);

    // dig root object
    const TValId rootAt = this->sh->valRoot(this->sh->placedAt(obj));

    const bool isVar = isProgramVar(this->sh->valTarget(rootAt));
    if (isVar)
        // colorize on-stack object
        this->dotStream << ", fontcolor=blue";

    else
        // colorize heap (sub)object
        this->dotStream << ", fontcolor=red";

    this->dotStream << ", label=\"";
    const TOffset off = this->sh->valOffset(this->sh->placedAt(obj));
    if (off < 0)
        this->dotStream << "[" << off << "] ";
    if (0 < off)
        this->dotStream << "[+" << off << "] ";
    this->dotStream << "[" << prefixByCode(code) << "] #" << obj;

    if (isVar) {
        CVar cVar = this->sh->cVarByRoot(rootAt);
        this->dotStream << " (#" << cVar.uid;
        const CodeStorage::Var &var = this->stor->vars[cVar.uid];
        std::string name = var.name;
        if (!name.empty())
            this->dotStream << " - " << name;
        if (1 < cVar.inst)
            this->dotStream << ", inst = " << cVar.inst;
        this->dotStream << ")";
    }

    std::string filedName;
    if (digFieldName(filedName, obj))
        this->dotStream << " ." << filedName;

    this->dotStream << "\"];" << std::endl;
}

void SymPlot::Private::plotNodeObjAnon(TObjId obj) {
    const TValId at = this->sh->placedAt(obj);
    const int size = this->sh->valSizeOfTarget(at);
    CL_BREAK_IF(size <= 0);

    this->dotStream << "\t" << SL_QUOTE(obj)
        << " [shape=box, color=red, fontcolor=red"
        << ", label=\"RAW " << size << "B\"];"
        << std::endl;
}

void SymPlot::Private::plotNodeValue(TValId val, enum cl_type_e code,
                                     const char *label)
{
    // visualize the count of references as pen width
    const float pw = static_cast<float>(this->sh->usedByCount(val));

    this->dotStream << "\t" << SL_QUOTE(val)
        << " [shape=ellipse"
        << ", penwidth=" << pw
        << ", color=" << colorByCode(code)
        << ", fontcolor=green"
        << ", label=\"[" << prefixByCode(code) << "] #" << val;

    if (label)
        this->dotStream << " [" << label << "]";

    this->dotStream << "\"];" << std::endl;
}

void SymPlot::Private::plotNodeAux(int src, enum cl_type_e code,
                                   const char *label)
{
    const int id = ++(this->last);
    this->dotStream << "\t"
        << SL_QUOTE("lonely" << id)
        << " [shape=plaintext"
        << ", fontcolor=" << colorByCode(code)
        << ", label=" << SL_QUOTE(label) << "];"
        << std::endl;

    this->dotStream << "\t"
        << SL_QUOTE(src) << " -> " << SL_QUOTE("lonely" << id)
        << " [color=" << colorByCode(code)
        << "];" << std::endl;
}

void SymPlot::Private::plotNeqZero(TValId val) {
    const char *label = "!= 0";

    const int id = ++(this->last);
    this->dotStream << "\t"
        << SL_QUOTE("lonely" << id)
        << " [shape=plaintext"
        << ", fontcolor=" << colorByCode(CL_TYPE_VOID)
        << ", label=" << SL_QUOTE(label) << "];"
        << std::endl;

    this->dotStream << "\t"
        << SL_QUOTE(val) << " -> " << SL_QUOTE("lonely" << id)
        << " [color=" << colorByCode(CL_TYPE_BOOL)
        << "];" << std::endl;
}

void SymPlot::Private::plotEdgePointsTo(TValId value, TObjId obj) {
    this->dotStream << "\t" << SL_QUOTE(value) << " -> " << SL_QUOTE(obj)
        << " [color=green, fontcolor=green, label=\"pointsTo\"];"
        << std::endl;
}

void SymPlot::Private::plotEdgeValueOf(TObjId obj, TValId value) {
    this->dotStream << "\t" << SL_QUOTE(obj) << " -> " << SL_QUOTE(value)
        << " [color=blue, fontcolor=blue];"
        << std::endl;
}

void SymPlot::Private::plotEdgeSub(TObjId obj, TObjId sub) {
    this->dotStream << "\t" << SL_QUOTE(obj) << " -> " << SL_QUOTE(sub)
        << " [color=gray, style=dotted, arrowhead=open"
        << ", fontcolor=gray, label=\"field\"];"
        << std::endl;
}

void SymPlot::Private::gobbleEdgeValueOf(TObjId obj, TValId value) {
    TEdgeValueOf edge(obj, value);
    this->evList.push_back(edge);
}

#if 0
void SymPlot::Private::gobbleEdgeOffValue(TValId val,
                                          const SymHeap::TOffVal &ov)
{
    TEdgeOffVal edge(val, ov);
    this->ovList.push_back(edge);
}
#endif

void SymPlot::Private::gobbleEdgeNeq(TValId val1, TValId val2) {
    // Neq predicates induce a symmetric relation, let's handle them such
    sortValues(val1, val2);

    TEdgeNeq edge(val1, val2);
    this->neqSet.insert(edge);
}

void SymPlot::Private::emitPendingEdges() {
    // plot all valueOf edges
    BOOST_FOREACH(const TEdgeValueOf &edge, this->evList) {
        this->plotEdgeValueOf(edge.first, edge.second);
    }

    // TODO: plot all off-value edges
#if 0
    BOOST_FOREACH(const TEdgeOffVal &edge, this->ovList) {
        const TValId dst = edge.first;
        const SymHeap::TOffVal &ov = edge.second;
        this->dotStream << "\t" << SL_QUOTE(ov.first) << " -> " << SL_QUOTE(dst)
            << " [color=red, fontcolor=red,"
            << " label=\"[+" << ov.second << "]\"];"
            << std::endl;
    }
#endif

    // plot Neq edges
    std::set<TEdgeNeq> neqDone;
    BOOST_FOREACH(const TEdgeNeq &edge, this->neqSet) {
        const TValId v1 = edge.first;
        const TValId v2 = edge.second;
        if (!insertOnce(neqDone, TEdgeNeq(v1, v2)))
            continue;

        this->dotStream << "\t" << SL_QUOTE(v1) << " -> " << SL_QUOTE(v2)
            << " [color=gold, fontcolor=red, label=\"Neq\", arrowhead=none];"
            << std::endl;
    }

    // cleanup for next wheel
    this->evList.clear();
    // this->ovList.clear();
    this->neqSet.clear();
}

// traverse all Neq predicates
void SymPlot::Private::traverseNeqs(TValId ref) {
    TValList relatedVals;
    this->sh->gatherRelatedValues(relatedVals, ref);
    BOOST_FOREACH(TValId val, relatedVals) {
        CL_BREAK_IF(val < 0);
        CL_BREAK_IF(!this->sh->SymHeapCore::proveNeq(ref, val));
        if (VAL_NULL == val) {
            // 'value' is said to be non-zero
            this->plotNeqZero(ref);
            continue;
        }

        // regular Neq predicate
        this->workList.schedule(val);
        this->gobbleEdgeNeq(ref, val);
    }
}

void SymPlot::Private::plotSingleValue(TValId value) {
    if (value <= 0) {
        this->plotNodeValue(value, CL_TYPE_UNKNOWN, 0);
        return;
    }

    // TODO: visualize off-value relations
#if 0
    SymHeap::TOffValCont offValues;
    this->sh->gatherOffValues(offValues, value);
    BOOST_FOREACH(const SymHeap::TOffVal &ov, offValues) {
        this->workList.schedule(ov.first);
        if (ov.second < 0)
            // we came to the predicate from the less interesting side; let's
            // just wait for the value on the opposite side of the predicate
            continue;

        this->gobbleEdgeOffValue(value, ov);
    }
#endif

    this->traverseNeqs(value);
    if (this->sh->valOffset(value))
        this->traverseNeqs(this->sh->valRoot(value));

    const struct cl_type *clt = 0;
    const TObjId target = const_cast<SymHeap *>(this->sh)->objAt(value);
    if (0 < target)
        clt = this->sh->objType(target);

    const enum cl_type_e code = (clt)
        ? clt->code
        : CL_TYPE_UNKNOWN;

    this->plotNodeValue(value, code, 0);
}

void SymPlot::Private::plotZeroValue(TObjId obj) {
    const struct cl_type *clt = this->sh->objType(obj);
    CL_BREAK_IF(!clt);

    const enum cl_type_e code = clt->code;
    switch (code) {
        case CL_TYPE_INT:
            this->plotNodeAux(obj, code, "[int] 0");
            break;

        case CL_TYPE_PTR:
            this->plotNodeAux(obj, code, "NULL");
            break;

        case CL_TYPE_BOOL:
            this->plotNodeAux(obj, code, "FALSE");
            break;

        default:
            this->plotNodeAux(obj, CL_TYPE_UNKNOWN, "[unknown type] 0");
    }
}

void SymPlot::Private::digNext(TObjId obj) {
    const TValId at = this->sh->placedAt(obj);

    const EObjKind kind = this->sh->valTargetKind(at);
    switch (kind) {
        case OK_CONCRETE:
            return;

        case OK_MAY_EXIST:
        case OK_SLS:
        case OK_DLS:
            if (this->sh->valOffset(at))
                return;
    }

    SymHeap &sh = *const_cast<SymHeap *>(this->sh);
    const BindingOff &off = segBinding(*this->sh, obj);
    const TOffset offHead = off.head;
    if (offHead) {
        const TObjId objHead = sh.objAt(sh.valByOffset(at, offHead));
        CL_BREAK_IF(objHead <= 0);

        // store 'head' pointer object
        this->heads.insert(objHead);
    }

    const TObjId objNext = sh.ptrAt(sh.valByOffset(at, off.next));
    CL_BREAK_IF(objNext <= 0);

    // store 'next' poitner object
    this->nexts.insert(objNext);
    if (OK_DLS != kind)
        return;

    const TObjId objPeer = sh.ptrAt(sh.valByOffset(at, off.prev));
    CL_BREAK_IF(objPeer <= 0);

    // store 'peer' pointer object
    this->peers.insert(objPeer);
}

void SymPlot::Private::openCluster(TObjId obj) {
    std::string label;
    const TValId at = this->sh->placedAt(obj);
    if (this->sh->valTargetIsProto(at))
        label = "[prototype] ";

#ifndef NDEBUG
    const char *color, *pw;
#else
    const char *color = "", *pw = "";
#endif

    const struct cl_type *clt = this->sh->objType(obj);
    CL_BREAK_IF(!clt);

    const EObjKind kind = this->sh->valTargetKind(at);
    switch (kind) {
        case OK_CONCRETE:
            color = (CL_TYPE_UNION == clt->code)
                ? "red"
                : "black";
            pw = "1.0";
            break;

        case OK_MAY_EXIST:
            label += "MAY_EXIST";
            color = "blue";
            pw = "3.0";
            break;

            // TODO
#if 0
        case OK_HEAD:
            label += "head";
            color = "green";
            pw = "2.0";
            break;
#endif

        case OK_SLS:
            label += "SLS";
            color = "red";
            pw = "3.0";
            break;

        case OK_DLS:
            label += "DLS/2";
            color = "gold";
            pw = "3.0";
            break;
    }

    this->dotStream
        << "subgraph \"cluster" <<(++last)<< "\" {" << std::endl
        << "\trank=same;"                           << std::endl
        << "\tlabel=" << SL_QUOTE(label) << ";"     << std::endl
        << "\tcolor=" << color << ";"               << std::endl
        << "\tfontcolor=" << color << ";"           << std::endl
        << "\tbgcolor=gray98;"                      << std::endl;

    this->dotStream
        << "\tstyle=dashed;"                        << std::endl
        << "\tpenwidth=" << pw << ";"               << std::endl;
}

bool SymPlot::Private::handleCustomValue(TValId value) {
    using namespace CodeStorage;

    if (VT_CUSTOM != this->sh->valTarget(value))
        return false;

    const int cVal = this->sh->valGetCustom(value);
    const CodeStorage::FncDb &fncs = this->stor->fncs;
    const Fnc *fnc = fncs[cVal];
    CL_BREAK_IF(!fnc);

    const char *fncName = nameOf(*fnc);
    CL_BREAK_IF(!fncName);

    std::string name(fncName);
    name += "()";

    this->plotNodeValue(value, CL_TYPE_FNC, 0);
    this->plotNodeAux(value, CL_TYPE_FNC, name.c_str());
    return true;
}

bool SymPlot::Private::handleUnknownValue(TValId value) {
    const EValueTarget code = this->sh->valTarget(value);
    switch (code) {
        case VT_STATIC:
        case VT_ON_STACK:
        case VT_ON_HEAP:
        case VT_ABSTRACT:
        case VT_CUSTOM:
            return false;

        case VT_LOST:
            this->plotNodeAux(value, CL_TYPE_UNKNOWN, "VT_LOST");
            return true;

        case VT_DELETED:
            this->plotNodeAux(value, CL_TYPE_UNKNOWN, "VT_DELETED");
            return true;

        case VT_UNKNOWN:
            this->plotNodeAux(value, CL_TYPE_UNKNOWN, "VT_UNKNOWN");
            return true;

        case VT_COMPOSITE:
        case VT_INVALID:
            break;
    }

    CL_BREAK_IF("unhandled value target code");
    return true;
}

bool SymPlot::Private::resolveValueOf(TValId *pDst, TObjId obj) {
    CL_BREAK_IF(obj < 0);

    // avoid duplicates
    if (hasKey(this->objDone, obj))
        return false;
    this->objDone.insert(obj);

    const TValId value = this->sh->valueOf(obj);
    switch (value) {
        case VAL_INVALID:
            this->plotNodeAux(obj, CL_TYPE_VOID, "VAL_INVALID");
            return false;

        case VAL_NULL /* = VAL_FALSE*/:
            this->plotZeroValue(obj);
            return false;

        case VAL_TRUE:
            this->plotNodeAux(obj, CL_TYPE_BOOL, "TRUE");
            return false;

        case VAL_DEREF_FAILED:
            this->plotNodeAux(obj, CL_TYPE_VOID, "UV_DEREF_FAILED");
            return false;

        default:
            break;
    }

    *pDst = value;
    return true;
}

bool SymPlot::Private::resolvePointsTo(TObjId *pDst, TValId value) {
    if (this->handleUnknownValue(value))
        return false;

    if (this->handleCustomValue(value))
        return false;

    const TObjId obj = const_cast<SymHeap *>(this->sh)->objAt(value);
    switch (obj) {
        case OBJ_INVALID:
            this->plotNodeAux(value, CL_TYPE_VOID, "INVALID");
            return false;

        case OBJ_DEREF_FAILED:
            this->plotNodeAux(value, CL_TYPE_VOID, "DEREF_FAILED");
            return false;

        case OBJ_UNKNOWN:
            this->plotNodeAux(value, CL_TYPE_UNKNOWN, "?");
            return false;

        case OBJ_RETURN:
        default:
            *pDst = obj;
            return true;
    }
}

class ObjectDigger {
    private:
        SymPlot::Private    *const self_;
        const TValId        root_;
        const TObjType      rootClt_;
        unsigned            level_;

    public:
        ObjectDigger(SymPlot::Private *self, TObjId root):
            self_(self),
            root_(self_->sh->placedAt(root)),
            rootClt_(self_->sh->objType(root)),
            level_(0)
        {
            this->operate(TFieldIdxChain(), rootClt_);
        }

        ~ObjectDigger() {
            // finally close all pending clusters
            this->setupNestLevel(0);
        }

        bool operator()(TFieldIdxChain ic, const struct cl_type_item *item) {
            this->operate(ic, item->type);
            return /* continue */ true;
        }

    private:
        void setupNestLevel(unsigned targetLevel) {
            for(; targetLevel < level_; --level_)
                self_->dotStream << "}" << std::endl;

            level_ = targetLevel;
        }

        void operate(TFieldIdxChain ic, const struct cl_type *clt);
};

void ObjectDigger::operate(TFieldIdxChain ic, const struct cl_type *clt) {
    const TOffset off = offsetByIdxChain(rootClt_, ic);

    SymHeap &sh = /* XXX */ *const_cast<SymHeap *>(self_->sh);
    const TValId at = sh.valByOffset(root_, off);
    const TObjId obj = sh.objAt(at, clt);
    CL_BREAK_IF(obj <= 0);

    // first close all pending clusters
    this->setupNestLevel(ic.size());

    if (!isComposite(clt)) {
        self_->plotNodeObj(obj);

        TValId value;
        if (self_->resolveValueOf(&value, obj)) {
            self_->gobbleEdgeValueOf(obj, value);
            self_->workList.schedule(value);
        }

        return;
    }

    // avoid duplicates
    self_->objDone.insert(obj);

    self_->digNext(obj);
    self_->openCluster(obj);
    self_->plotNodeObj(obj);
    for (int i = 0; i < clt->item_cnt; ++i) {
        const TObjId sub = self_->sh->subObj(obj, i);
        const struct cl_type *subClt = self_->sh->objType(sub);
        if (!subClt || (isComposite(subClt) && !subClt->item_cnt))
            // skip empty structures/unions
            continue;

        if (!hasKey(self_->objDone, sub))
            self_->plotEdgeSub(obj, sub);
    }
}

void SymPlot::Private::digObjCore(TObjId obj) {
    const struct cl_type *clt = this->sh->objType(obj);
    if (!clt) {
        this->plotNodeObjAnon(obj);
        return;
    }

    ObjectDigger visitor(this, obj);
    traverseTypeIc(clt, visitor, /* digOnlyStructs */ true);
}

void SymPlot::Private::digObj(TObjId obj) {
    SymHeap &writable = *const_cast<SymHeap *>(this->sh);

    // seek root, in order to draw the whole object, even if the root is not
    // pointed from anywhere
    const TValId addr = this->sh->valRoot(this->sh->placedAt(obj));
    obj = writable.objAt(addr);

    if (OK_DLS != this->sh->valTargetKind(addr)) {
        this->digObjCore(obj);
        return;
    }

    const TObjId peer = writable.objAt(dlSegPeer(writable, addr));
    const char *label = (this->sh->valTargetIsProto(addr))
        ? "[prototype] DLS"
        : "DLS";

    // open a cluster
    this->dotStream
        << "subgraph \"cluster" <<(++last)<< "\" {" << std::endl
        << "\tlabel=" << SL_QUOTE(label)            << std::endl
        << "\tcolor=gold;"                          << std::endl
        << "\tfontcolor=gold;"                      << std::endl
        << "\tstyle=dashed;"                        << std::endl;

    // plot the two parts of a DLS into the cluster
    this->digObjCore(obj);
    if (0 < peer)
        this->digObjCore(peer);

    // close the cluster
    this->dotStream << "}" << std::endl;
}

void SymPlot::Private::digValues() {
    TValId value;
    while (workList.next(value)) {
        // plot the value itself
        this->plotSingleValue(value);

        if (value <= 0)
            // bare value can't be followed
            continue;

        if (VT_COMPOSITE == this->sh->valTarget(value)) {
            // dig composite object and eventually schedule the values inside
            const TObjId cObj = this->sh->valGetComposite(value);
            this->digObj(cObj);
            continue;
        }

        // check the value inside
        TObjId obj;
        if (!this->resolvePointsTo(&obj, value))
            // bare value can't be followed
            continue;

        // plot the pointing object and the corresponding "valueOf" edge
        this->plotNodeObj(obj);
        this->plotEdgePointsTo(value, obj);

        // follow values inside the object
        this->digObj(obj);
    }
}

void SymPlot::Private::plotObj(TObjId obj) {
    // plot the variable itself
    this->plotNodeObj(obj);

    // look for the value inside
    TValId value;
    if (!this->resolveValueOf(&value, obj))
        // we got a bare value, which can't be followed, so we're done
        return;

    if (isComposite(this->sh->objType(obj))) {
        // connect the variable node with its value
        this->plotEdgeValueOf(obj, value);

        // dig the target value recursively and plot (if not already)
        this->workList.schedule(value);
    }
    else
        // dig composite object and eventually schedule the values inside
        this->digObj(obj);

    this->digValues();
}

void SymPlot::Private::plotCVar(CVar cVar) {
    // CodeStorage variable lookup
    const CodeStorage::Var &var = this->stor->vars[cVar.uid];
    this->lw = &var.loc;
#if DEBUG_SYMPLOT
    CL_DEBUG_MSG(this->lw, "-X- plotting stack variable: #" << var.uid
            << " (" << var.name << ")" );
#endif

    // SymbolicHeap variable lookup
    SymHeap &writable = *const_cast<SymHeap *>(this->sh);
    const TValId at = writable.addrOfVar(cVar);
    const TObjId obj = writable.objAt(at);
    if (OBJ_INVALID == obj)
        CL_DEBUG_MSG(this->lw, "objByCVar lookup failed");

    // plot as regular heap object
    this->plotObj(obj);
}

SymPlot::SymPlot(const SymHeap &sh):
    d(new Private)
{
    d->sh = &sh;
    d->stor = &sh.stor();
    d->last = 0;
}

SymPlot::~SymPlot() {
    delete d;
}

bool SymPlot::plot(const std::string &name) {
    // create dot file
    d->ok = true;
    if (!d->openDotFile(name))
        return false;

    // go through all program variables
    TCVarList cVars;
    d->sh->gatherCVars(cVars);
    BOOST_FOREACH(CVar cv, cVars) {
        d->plotCVar(cv);
    }

    // plot also all dangling objects, although we are not happy to see them
    TValList roots;
    d->sh->gatherRootObjects(roots);
    BOOST_FOREACH(const TValId at, roots) {
        const TObjId obj = const_cast<SymHeap *>(d->sh)->objAt(at);
        if (!hasKey(d->objDone, obj))
            d->plotObj(obj);
    }

    // close dot file
    d->closeDotFile();
    return d->ok;
}

bool SymPlot::plotHeapValue(const std::string &name, TValId value) {
    // create dot file
    d->ok = true;
    if (!d->openDotFile(name))
        return false;

    // plot by value
    d->workList.schedule(value);
    d->digValues();

    // close dot file
    d->closeDotFile();
    return d->ok;
}

bool SymPlot::plotStackFrame(const std::string           &name,
                             const CodeStorage::Fnc      &fnc,
                             const SymBackTrace          *bt)
{
    using namespace CodeStorage;

    // create dot file
    d->ok = true;
    if (!d->openDotFile(name))
        return false;

    d->lw = &fnc.def.loc;
#if DEBUG_SYMPLOT
    CL_DEBUG_MSG(d->lw, "-X- plotting stack frame of " << nameOf(fnc) << "():");
#endif

    // go through all stack variables
    BOOST_FOREACH(const int uid, fnc.vars) {
        const int nestLevel = bt->countOccurrencesOfFnc(uidOf(fnc));
        const CVar cVar(uid, nestLevel);
        d->plotCVar(cVar);
    }

    // close dot file
    d->closeDotFile();
    return d->ok;
}

// vim: tw=80
