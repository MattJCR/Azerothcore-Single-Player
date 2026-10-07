// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <functional>
#include <map>
#include <string>
#include <variant>
template<class E> class ConfigValueCache
{
    std::map<E,std::variant<bool,float>> values;
public:
    enum class Reloadable { Yes, No };
    explicit ConfigValueCache(E) {}
    virtual ~ConfigValueCache() = default;
    virtual void BuildConfigCache() = 0;
    void Initialize(bool) { BuildConfigCache(); }
    template<class T> void SetConfigValue(E e, std::string const&, T const& v,
        Reloadable=Reloadable::Yes, std::function<bool(T const&)>&& = {}, std::string const& = "") { values[e]=v; }
    template<class T> T GetConfigValue(E e) const { return std::get<T>(values.at(e)); }
    template<class T> void OverwriteConfigValue(E e, T const& v) { values[e]=v; }
};
