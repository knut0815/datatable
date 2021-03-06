//------------------------------------------------------------------------------
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// © H2O.ai 2018
//------------------------------------------------------------------------------
#include <type_traits>
#include "column.h"
#include "utils.h"
#include "utils/assert.h"
#include "utils/omp.h"



/**
 * Private constructor that creates an "invalid" column. An `init_X` method
 * should be subsequently called before using this column.
 */
template <typename T>
FwColumn<T>::FwColumn() : Column(0) {}

template <typename T>
FwColumn<T>::FwColumn(int64_t nrows_, MemoryBuffer* mb) : Column(nrows_) {
  size_t req_size = elemsize() * static_cast<size_t>(nrows_);
  if (mb == nullptr) {
    mb = new MemoryMemBuf(req_size);
  } else {
    xassert(mb->size() == req_size);
  }
  mbuf = mb;
}


//==============================================================================
// Initialization methods
//==============================================================================

template <typename T>
void FwColumn<T>::init_data() {
  xassert(!ri && !mbuf);
  mbuf = new MemoryMemBuf(static_cast<size_t>(nrows) * elemsize());
}

template <typename T>
void FwColumn<T>::init_mmap(const std::string& filename) {
  xassert(!ri && !mbuf);
  mbuf = new MemmapMemBuf(filename, static_cast<size_t>(nrows) * elemsize());
}

template <typename T>
void FwColumn<T>::open_mmap(const std::string& filename) {
  xassert(!ri && !mbuf);
  mbuf = new MemmapMemBuf(filename);
  size_t exp_size = static_cast<size_t>(nrows) * sizeof(T);
  if (mbuf->size() != exp_size) {
    throw Error() << "File \"" << filename <<
        "\" cannot be used to create a column with " << nrows <<
        " rows. Expected file size of " << exp_size <<
        " bytes, actual size is " << mbuf->size() << " bytes";
  }
}

template <typename T>
void FwColumn<T>::init_xbuf(Py_buffer* pybuffer) {
  xassert(!ri && !mbuf);
  size_t exp_buf_len = static_cast<size_t>(nrows) * elemsize();
  if (static_cast<size_t>(pybuffer->len) != exp_buf_len) {
    throw Error() << "PyBuffer cannot be used to create a column of " << nrows
                  << " rows: buffer length is " << static_cast<size_t>(pybuffer->len)
                  << ", expected " << exp_buf_len;
  }
  mbuf = new ExternalMemBuf(pybuffer->buf, pybuffer, exp_buf_len);
}



//==============================================================================

template <typename T>
void FwColumn<T>::replace_buffer(MemoryBuffer* new_mbuf, MemoryBuffer*) {
  xassert(new_mbuf != nullptr);
  if (new_mbuf->size() % sizeof(T)) {
    throw RuntimeError() << "New buffer has invalid size " << new_mbuf->size();
  }
  if (mbuf) mbuf->release();
  mbuf = new_mbuf;
  nrows = static_cast<int64_t>(mbuf->size() / sizeof(T));
}

template <typename T>
size_t FwColumn<T>::elemsize() const {
  return sizeof(T);
}

template <typename T>
bool FwColumn<T>::is_fixedwidth() const {
  return true;
}


template <typename T>
T* FwColumn<T>::elements() const {
  return static_cast<T*>(mbuf->get());
}


template <typename T>
T FwColumn<T>::get_elem(int64_t i) const {
  return mbuf->get_elem<T>(i);
}


template <>
void FwColumn<PyObject*>::set_elem(int64_t i, PyObject* value) {
  mbuf->set_elem<PyObject*>(i, value);
  Py_XINCREF(value);
}

template <typename T>
void FwColumn<T>::set_elem(int64_t i, T value) {
  mbuf->set_elem<T>(i, value);
}


template <typename T>
void FwColumn<T>::reify() {
  // If the rowindex is absent, then there's nothing else left to do.
  if (ri.isabsent()) return;

  size_t elemsize = sizeof(T);
  size_t nrows_cast = static_cast<size_t>(nrows);
  size_t newsize = elemsize * nrows_cast;

  // Current `mbuf` can be reused iff it is not readonly. Thus, `new_mbuf` can
  // be either the same as `mbuf` (with old size), or a newly allocated buffer
  // (with new size). Correspondingly, the old buffer may or may not have to be
  // released afterwards.
  // Note also that `newsize` may be either smaller or bigger than the old size,
  // this must be taken into consideration.
  auto new_mbuf = mbuf->is_readonly()? new MemoryMemBuf(newsize) : mbuf;

  if (ri.isslice() && ri.slice_step() == 1) {
    // Slice with step 1: a portion of the buffer can be simply mem-moved onto
    // the new buffer (use memmove because the old and the new buffer can be
    // the same).
    size_t start = static_cast<size_t>(ri.slice_start());
    xassert(newsize + start * elemsize <= mbuf->size());
    memmove(new_mbuf->get(), elements() + start, newsize);

  } else {
    // In all other cases we have to manually loop over the rowindex and
    // copy array elements onto the new positions. This can be done in-place
    // only if we know that the indices are monotonically increasing (otherwise
    // there is a risk of scrambling the data).
    if (mbuf == new_mbuf && !(ri.isslice() && ri.slice_step() > 0)) {
      new_mbuf = new MemoryMemBuf(newsize);
    }
    T* data_src = elements();
    T* data_dest = static_cast<T*>(new_mbuf->get());
    ri.strided_loop(0, nrows, 1,
      [&](int64_t i) {
        *data_dest = data_src[i];
        ++data_dest;
      });
  }

  if (mbuf == new_mbuf) {
    new_mbuf->resize(newsize);
  } else {
    mbuf->release();
    mbuf = new_mbuf;
  }
  ri.clear(true);
}



// The purpose of this template is to augment the behavior of the template
// `resize_and_fill` method, for the case when `T` is `PyObject*`: if a
// `PyObject*` value is replicated multiple times in a column, then its
// refcount has to be increased that many times.
template <typename T> void incr_refcnt(T, int64_t) {}
template <> void incr_refcnt(PyObject* obj, int64_t drefcnt) {
  if (obj) Py_REFCNT(obj) += drefcnt;
}


template <typename T>
void FwColumn<T>::resize_and_fill(int64_t new_nrows)
{
  if (new_nrows == nrows) return;
  if (new_nrows < nrows) {
    throw RuntimeError() << "Column::resize_and_fill() cannot shrink a column";
  }

  mbuf = mbuf->safe_resize(sizeof(T) * static_cast<size_t>(new_nrows));

  // Replicate the value or fill with NAs
  T fill_value = nrows == 1? get_elem(0) : na_elem;
  for (int64_t i = nrows; i < new_nrows; ++i) {
    mbuf->set_elem<T>(i, fill_value);
  }
  incr_refcnt<T>(fill_value, new_nrows - nrows);
  this->nrows = new_nrows;

  // TODO(#301): Temporary fix.
  if (this->stats != nullptr) this->stats->reset();
}


template <typename T>
void FwColumn<T>::rbind_impl(std::vector<const Column*>& columns,
                             int64_t new_nrows, bool col_empty)
{
  const T na = na_elem;
  const void *naptr = static_cast<const void*>(&na);

  // Reallocate the column's data buffer
  size_t old_nrows = (size_t) nrows;
  size_t old_alloc_size = alloc_size();
  size_t new_alloc_size = sizeof(T) * (size_t) new_nrows;
  mbuf = mbuf->safe_resize(new_alloc_size);
  xassert(!mbuf->is_readonly());
  nrows = new_nrows;

  // Copy the data
  void *resptr = mbuf->at(col_empty ? 0 : old_alloc_size);
  size_t rows_to_fill = col_empty ? old_nrows : 0;
  for (const Column* col : columns) {
    if (col->stype() == 0) {
      rows_to_fill += (size_t) col->nrows;
    } else {
      if (rows_to_fill) {
        set_value(resptr, naptr, sizeof(T), rows_to_fill);
        resptr = add_ptr(resptr, rows_to_fill * sizeof(T));
        rows_to_fill = 0;
      }
      if (col->stype() != stype()) {
        Column *newcol = col->cast(stype());
        delete col;
        col = newcol;
      }
      memcpy(resptr, col->data(), col->alloc_size());
      resptr = add_ptr(resptr, col->alloc_size());
    }
    delete col;
  }
  if (rows_to_fill) {
    set_value(resptr, naptr, sizeof(T), rows_to_fill);
    resptr = add_ptr(resptr, rows_to_fill * sizeof(T));
  }
  xassert(resptr == mbuf->at(new_alloc_size));
}


template <typename T>
int64_t FwColumn<T>::data_nrows() const {
  return static_cast<int64_t>(mbuf->size() / sizeof(T));
}


template <typename T>
void FwColumn<T>::apply_na_mask(const BoolColumn* mask) {
  const int8_t* maskdata = mask->elements();
  constexpr T na = GETNA<T>();
  T* coldata = this->elements();
  #pragma omp parallel for schedule(dynamic, 1024)
  for (int64_t j = 0; j < nrows; ++j) {
    if (maskdata[j] == 1) coldata[j] = na;
  }
  if (stats != nullptr) stats->reset();
}

template <typename T>
void FwColumn<T>::fill_na() {
  // `reify` will copy extraneous data, so we implement our own reification here
  if (mbuf->is_readonly()) {
    mbuf->release();
    mbuf = new MemoryMemBuf(static_cast<size_t>(nrows) * elemsize());
  }
  T* vals = static_cast<T*>(mbuf->get());
  T na = GETNA<T>();
  #pragma omp parallel for schedule(static)
  for (int64_t i = 0; i < nrows; ++i) {
    vals[i] = na;
  }
  ri.clear(false);
}


// Explicit instantiations
template class FwColumn<int8_t>;
template class FwColumn<int16_t>;
template class FwColumn<int32_t>;
template class FwColumn<int64_t>;
template class FwColumn<float>;
template class FwColumn<double>;
template class FwColumn<PyObject*>;
