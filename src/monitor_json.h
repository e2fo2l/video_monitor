// MonitorJson — thin wrapper around a rapidjson object document.
#pragma once

#include <string>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

class MonitorJson : public rapidjson::Document
{
public:
  // 0x469f34
  MonitorJson()
      : rapidjson::Document(rapidjson::kObjectType)
  {
  }

  // 0x469fa8
  std::string ToString() const
  {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    Accept(writer);
    return buf.GetString();
  }

  // 0x47d8b8
  void AddString(const std::string& key, const std::string& value)
  {
    auto& a = GetAllocator();
    rapidjson::Value k, v;
    k.SetString(key.data(), static_cast<rapidjson::SizeType>(key.size()), a);
    v.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), a);
    AddMember(k, v, a);
  }

  // 0x47d9c4 (stores as int64 even though it is called AddInt)
  void AddInt(const std::string& key, long value) { AddInt64(key, value); }

  // 0x47daa4
  void AddInt64(const std::string& key, long value)
  {
    auto& a = GetAllocator();
    rapidjson::Value k, v;
    k.SetString(key.data(), static_cast<rapidjson::SizeType>(key.size()), a);
    v.SetInt64(value);
    AddMember(k, v, a);
  }
};
