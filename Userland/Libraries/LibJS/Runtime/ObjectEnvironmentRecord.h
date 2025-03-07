/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/EnvironmentRecord.h>

namespace JS {

class ObjectEnvironmentRecord : public EnvironmentRecord {
    JS_OBJECT(ObjectEnvironmentRecord, EnvironmentRecord);

public:
    ObjectEnvironmentRecord(Object&, EnvironmentRecord* parent_scope);

    virtual Optional<Variable> get_from_environment_record(FlyString const&) const override;
    virtual void put_into_environment_record(FlyString const&, Variable) override;
    virtual bool delete_from_environment_record(FlyString const&) override;

    Object& object() { return m_object; }

private:
    virtual void visit_edges(Visitor&) override;

    Object& m_object;
};

}
