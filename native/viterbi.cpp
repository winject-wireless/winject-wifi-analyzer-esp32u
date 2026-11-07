// Fast hard-decision K=7 convolutional decoder for IEEE 802.11.
// Input bytes: 0 and 1 are hard decisions, 2 denotes a punctured erasure.
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <vector>

static PyObject* viterbi(PyObject*, PyObject* args) {
    PyObject* input;
    int force_zero;
    if (!PyArg_ParseTuple(args, "Op:viterbi", &input, &force_zero)) return nullptr;
    Py_buffer view{};
    if (PyObject_GetBuffer(input, &view, PyBUF_CONTIG_RO | PyBUF_FORMAT) < 0) return nullptr;
    if (view.itemsize != 1 || view.len % 2 || (view.format && view.format[0] != 'B' && view.format[0] != 'b')) {
        PyBuffer_Release(&view); PyErr_SetString(PyExc_ValueError, "coded bits must be a contiguous, even-length byte buffer"); return nullptr;
    }
    const auto* coded = static_cast<const uint8_t*>(view.buf);
    for (Py_ssize_t i=0;i<view.len;i++) {
        if (coded[i] > 2) { PyBuffer_Release(&view); PyErr_SetString(PyExc_ValueError, "coded bits must be 0, 1, or erasure 2"); return nullptr; }
    }
    const size_t count = static_cast<size_t>(view.len / 2);
    // A PHY frame cannot need an unbounded history; guard allocation overflow.
    if (count > 1000000) { PyBuffer_Release(&view); PyErr_SetString(PyExc_ValueError, "coded frame too large"); return nullptr; }
    PyObject* result = PyBytes_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(count));
    if (!result) { PyBuffer_Release(&view); return nullptr; }
    std::vector<uint8_t> history;
    try { history.resize(count * 64); }
    catch (const std::bad_alloc&) { Py_DECREF(result); PyBuffer_Release(&view); return PyErr_NoMemory(); }
    auto* out = reinterpret_cast<uint8_t*>(PyBytes_AS_STRING(result));
    Py_BEGIN_ALLOW_THREADS
    std::array<uint8_t,64> p0{},p1{},a0{},a1{},b0{},b1{};
    for (int s=0;s<64;s++) {
        p0[s]=s>>1;p1[s]=p0[s]|32;int bit=s&1;
        int r0=(p0[s]<<1)|bit,r1=(p1[s]<<1)|bit;
        a0[s]=__builtin_parity(r0&0155);a1[s]=__builtin_parity(r0&0117);
        b0[s]=__builtin_parity(r1&0155);b1[s]=__builtin_parity(r1&0117);
    }
    std::array<int,64> metric{},next{};metric.fill(1000000);metric[0]=0;
    for (size_t i=0;i<count;i++) {
        const uint8_t c0=coded[2*i],c1=coded[2*i+1];
        for (int s=0;s<64;s++) {
            int m0=metric[p0[s]]+(c0!=2 && a0[s]!=c0)+(c1!=2 && a1[s]!=c1);
            int m1=metric[p1[s]]+(c0!=2 && b0[s]!=c0)+(c1!=2 && b1[s]!=c1);
            bool choose=m1<m0;history[i*64+s]=choose;next[s]=choose?m1:m0;
        }
        metric=next;
    }
    int state=force_zero?0:int(std::min_element(metric.begin(),metric.end())-metric.begin());
    for (size_t i=count;i>0;i--) {out[i-1]=state&1;state=(state>>1)|(history[(i-1)*64+state]?32:0);}
    Py_END_ALLOW_THREADS
    PyBuffer_Release(&view);
    return result;
}

static PyObject* descramble(PyObject*, PyObject* input) {
    Py_buffer view{};
    if (PyObject_GetBuffer(input, &view, PyBUF_CONTIG_RO) < 0) return nullptr;
    if (view.itemsize != 1 || view.len < 16) {
        PyBuffer_Release(&view); PyErr_SetString(PyExc_ValueError, "descrambler needs at least 16 bits"); return nullptr;
    }
    const auto* bits = static_cast<const uint8_t*>(view.buf);
    int seed_found = 0, matches = 0;
    for (int seed=1; seed<128; ++seed) {
        int state=seed; bool valid=true;
        for (int i=0;i<16;++i) {
            int feedback=((state>>6)^(state>>3))&1;
            if (bits[i]!=feedback) {valid=false; break;}
            state=((state<<1)|feedback)&127;
        }
        if (valid) {seed_found=seed; ++matches;}
    }
    if (matches != 1) {PyBuffer_Release(&view); Py_RETURN_NONE;}
    PyObject* result=PyBytes_FromStringAndSize(nullptr, view.len);
    if (!result) {PyBuffer_Release(&view); return nullptr;}
    auto* out=reinterpret_cast<uint8_t*>(PyBytes_AS_STRING(result));
    int state=seed_found;
    for (Py_ssize_t i=0;i<view.len;++i) {
        int feedback=((state>>6)^(state>>3))&1;
        out[i]=bits[i]^feedback; state=((state<<1)|feedback)&127;
    }
    PyBuffer_Release(&view); return result;
}

static PyMethodDef methods[] = {{"viterbi", viterbi, METH_VARARGS, "Decode hard bits with optional puncture erasures."}, {"descramble", descramble, METH_O, "Descramble legacy OFDM bits."}, {nullptr,nullptr,0,nullptr}};
static PyModuleDef module = {PyModuleDef_HEAD_INIT, "_native", "Native WiFi decoder primitives.", -1, methods};
PyMODINIT_FUNC PyInit__native(void) { return PyModule_Create(&module); }
