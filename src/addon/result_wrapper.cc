#include <iostream>
#include "result_wrapper.h"
#include "duckdb.hpp"
using namespace std;

Napi::FunctionReference ResultWrapper::constructor;

Napi::Object ResultWrapper::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func =
      DefineClass(env,
                  "ResultWrapper",
                  {
                    InstanceMethod("fetchRow", &ResultWrapper::FetchRow),
                    InstanceMethod("describe", &ResultWrapper::Describe),
                  });

  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();

  exports.Set("ResultWrapper", func);
  return exports;
}

ResultWrapper::ResultWrapper(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ResultWrapper>(info) {
  Napi::Env env = info.Env();
}

Napi::Object ResultWrapper::Create(const Napi::CallbackInfo& info) {
  return constructor.New({});
}

typedef uint64_t idx_t;

int32_t GetDate(int64_t timestamp) {
	return (int32_t)(((int64_t)timestamp) >> 32);
}

int32_t GetTime(int64_t timestamp) {
	return (int32_t)(timestamp & 0xFFFFFFFF);
}

#define EPOCH_DATE 719528
#define SECONDS_PER_DAY (60 * 60 * 24)

int64_t Epoch(int32_t date) {
	return ((int64_t)date - EPOCH_DATE) * SECONDS_PER_DAY;
}

Napi::Value ResultWrapper::FetchRow(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!result) {
    Napi::RangeError::New(env, "Result closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }  
  if (!current_chunk || chunk_offset >= current_chunk->size()) {
    current_chunk = result->Fetch();
    chunk_offset = 0;
  }
  if (result->type == duckdb::QueryResultType::STREAM_RESULT) {
    cout << "Streaming!";
  } else {
    cout << "Materialized!";
  }
  if (!current_chunk) {
    Napi::Error::New(env, result->error).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (current_chunk->size() == 0) {
    return env.Null();
  }
  idx_t col_count = result->types.size();
  Napi::Array row = Napi::Array::New(env, col_count);

  for (idx_t col_idx = 0; col_idx < col_count; col_idx++) {
    auto &nullmask = duckdb::FlatVector::Nullmask(current_chunk->data[col_idx]);
    if (nullmask[chunk_offset]) {
      row.Set(col_idx, env.Null());
      continue;
    }
    auto val = current_chunk->data[col_idx].GetValue(chunk_offset);
    switch (result->sql_types[col_idx].id) {
    case duckdb::SQLTypeId::BOOLEAN:
      row.Set(col_idx, Napi::Boolean::New(env, val.GetValue<bool>()));
      break;
    case duckdb::SQLTypeId::TINYINT:
      row.Set(col_idx, Napi::Number::New(env, val.GetValue<int8_t>()));
      break;
    case duckdb::SQLTypeId::SMALLINT:
      row.Set(col_idx, Napi::Number::New(env, val.GetValue<int16_t>()));
      break;
    case duckdb::SQLTypeId::INTEGER:
      row.Set(col_idx, Napi::Number::New(env, val.GetValue<int32_t>()));
      break;
    case duckdb::SQLTypeId::BIGINT:
      #ifdef NAPI_EXPERIMENTAL
        row.Set(col_idx, Napi::BigInt::New(env, val.GetValue<int64_t>()));
      #else
        row.Set(col_idx, Napi::Number::New(env, val.GetValue<int64_t>()));
      #endif
      break;
    case duckdb::SQLTypeId::FLOAT:
      row.Set(col_idx, Napi::Number::New(env, val.GetValue<float>()));
      break;
    case duckdb::SQLTypeId::DOUBLE:
      row.Set(col_idx, Napi::Number::New(env, val.GetValue<double>()));
      break;
    case duckdb::SQLTypeId::VARCHAR:
      row.Set(col_idx, Napi::String::New(env, val.GetValue<string>()));
      break;

    case duckdb::SQLTypeId::TIMESTAMP: {
      if (result->types[col_idx] != duckdb::TypeId::INT64) {
        throw runtime_error("expected int64 for timestamp");
      }
      int64_t tval = val.GetValue<int64_t>();
      int64_t date = Epoch(GetDate(tval)) * 1000;
      int32_t time = GetTime(tval);
      row.Set(col_idx, Napi::Number::New(env, date + time));
      break;
    }
    case duckdb::SQLTypeId::DATE: {
      if (result->types[col_idx] != duckdb::TypeId::INT32) {
        throw runtime_error("expected int32 for date");
      }
      row.Set(col_idx, Napi::Number::New(env, Epoch(val.GetValue<int32_t>()) * 1000));
      break;
    }
    case duckdb::SQLTypeId::TIME: {
      if (result->types[col_idx] != duckdb::TypeId::INT32) {
        throw runtime_error("expected int32 for time");
      }
      int64_t tval = val.GetValue<int64_t>();      
      row.Set(col_idx, Napi::Number::New(env, GetTime(tval)));
      break;
    }

    default:
      throw runtime_error("unsupported type: " + SQLTypeToString(result->sql_types[col_idx]));
    }
  }
  chunk_offset++;
  return row;
}

Napi::Value ResultWrapper::Describe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!result) {
    Napi::TypeError::New(env, "Result closed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  idx_t col_count = result->names.size();
  Napi::Array row = Napi::Array::New(env, col_count);
  idx_t name_idx = 0;
  idx_t type_idx = 1;
  for (idx_t col_idx = 0; col_idx < col_count; col_idx++) {
    Napi::Array col = Napi::Array::New(env, 2);
    col.Set(name_idx, Napi::String::New(env, result->names[col_idx]));
    col.Set(type_idx, Napi::String::New(env, duckdb::SQLTypeToString(result->sql_types[col_idx])));
    row.Set(col_idx, col);
  }
  return row;
}
