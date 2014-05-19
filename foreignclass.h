#pragma once

#include <QVariant>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QFlag>
#include "common.h"

class QByteArray;

namespace MetaMetaObject {

class Object;
class MetaObject;

class ForeignClass : public std::enable_shared_from_this<ForeignClass>
{
public:
    ForeignClass(const SP<ForeignClass> &superclass = SP<ForeignClass>());
    virtual ~ForeignClass();

    struct Method
    {
        enum class Access
        {
            Private,
            Protected,
            Public
        };

        enum class Type
        {
            Method,
            Signal
        };

        QByteArray name;
        int arity;
        Access access;
        Type type;
        std::size_t id;
    };

    struct Property
    {
        enum class Flag
        {
            Readable = 1,
            Writable = 1 << 1,
            Constant = 1 << 2,
        };
        Q_DECLARE_FLAGS(Flags, Flag)

        QByteArray name;
        Flags flags;
        std::size_t id;
        bool hasNotifySignal;
        std::size_t notifySignalId;
    };

    QByteArray className() const { return mClassName; }
    QList<Method> methods() const { return mMethods; }
    QList<Method> signalMethods() const;
    QList<Method> nonSignalMethods() const;
    QList<Property> properties() const { return mProperties; }
    SP<ForeignClass> superClass() const { return mSuperclass; }

    SP<MetaObject> metaObject();

    void emitSignal(Object *obj, std::size_t id, const QVariantList &args);

    virtual QVariant callMethod(Object *obj, size_t id, const QVariantList &args) = 0;
    virtual void setProperty(Object *obj, size_t id, const QVariant &value) = 0;
    virtual QVariant getProperty(Object *obj, size_t id) = 0;

protected:

    void setClassName(const QByteArray &name) { mClassName = name; }
    void addMethod(const QByteArray &name, std::size_t id, int arity, Method::Access access = Method::Access::Public, Method::Type type = Method::Type::Method);
    void addSignal(const QByteArray &name, std::size_t id, int arity);
    void addProperty(const QByteArray &name, std::size_t id, Property::Flags flags, bool hasNotifySignal = false, std::size_t notifySignalId = 0);

private:

    SP<ForeignClass> mSuperclass;
    QByteArray mClassName;
    QList<Method> mMethods;
    QList<Property> mProperties;
    SP<MetaObject> mMetaObject;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(ForeignClass::Property::Flags)

} // namespace MetaMetaObject
