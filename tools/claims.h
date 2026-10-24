#pragma once
// Triples of the claims of an entity's JSON (see triples.h)
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "triples.h"

// For each claim P.. of an item, the main snak's value when it is an item,
// property or lexeme (datavalue.value: entity-type, numeric-id), with the
// statement's rank. Returns the number of triples added; nothing for other
// entities than items. Values of other entity types (forms, senses, entity
// schemas: no numeric-id) are counted in otherTargets.
inline size_t appendTriples(const nlohmann::json &entity, std::vector<Triple> &triples, size_t *otherTargets = nullptr) {
    const auto id = entity.find("id");
    const auto claims = entity.find("claims");
    if (id == entity.end() || !id->is_string() || claims == entity.end() || !claims->is_object())
        return 0;
    const auto &symbol = id->get_ref<const std::string &>();
    if (!symbol.starts_with('Q'))
        return 0;
    const auto subject = static_cast<uint32_t>(std::stoul(symbol.substr(1)));
    size_t added = 0;
    for (const auto &[property, statements] : claims->items()) {
        if (!property.starts_with('P') || !statements.is_array())
            continue;
        const auto p = static_cast<uint32_t>(std::stoul(property.substr(1)));
        for (const auto &statement : statements) {
            const auto snak = statement.find("mainsnak");
            if (snak == statement.end())
                continue;
            const auto datavalue = snak->find("datavalue");
            if (datavalue == snak->end() || !datavalue->contains("value"))
                continue;
            const auto &value = (*datavalue)["value"];
            if (!value.is_object())
                continue;
            const auto number = value.find("numeric-id");
            if (number == value.end() || !number->is_number_unsigned())
                continue;
            const auto entityType = value.value("entity-type", "item");
            TargetType type;
            if (entityType == "item")
                type = TargetType::item;
            else if (entityType == "property")
                type = TargetType::property;
            else if (entityType == "lexeme")
                type = TargetType::lexeme;
            else {
                if (otherTargets)
                    ++*otherTargets;
                continue;
            }
            const auto rankName = statement.value("rank", "normal");
            const Rank rank = rankName == "preferred"    ? Rank::preferred
                              : rankName == "deprecated" ? Rank::deprecated
                                                         : Rank::normal;
            triples.push_back(Triple::make(p, subject, rank, number->get<uint32_t>(), type));
            ++added;
        }
    }
    return added;
}
