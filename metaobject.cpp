#include "metaobject.h"
#include "foreignclass.h"
#include "object.h"
#include <QQueue>

namespace MetaMetaObject {

namespace {

// from qmetaobject_p.h in Qt 5.2.0

enum PropertyFlags  {
    Invalid = 0x00000000,
    Readable = 0x00000001,
    Writable = 0x00000002,
    Resettable = 0x00000004,
    EnumOrFlag = 0x00000008,
    StdCppSet = 0x00000100,
//     Override = 0x00000200,
    Constant = 0x00000400,
    Final = 0x00000800,
    Designable = 0x00001000,
    ResolveDesignable = 0x00002000,
    Scriptable = 0x00004000,
    ResolveScriptable = 0x00008000,
    Stored = 0x00010000,
    ResolveStored = 0x00020000,
    Editable = 0x00040000,
    ResolveEditable = 0x00080000,
    User = 0x00100000,
    ResolveUser = 0x00200000,
    Notify = 0x00400000,
    Revisioned = 0x00800000
};

enum MethodFlags  {
    AccessPrivate = 0x00,
    AccessProtected = 0x01,
    AccessPublic = 0x02,
    AccessMask = 0x03, //mask

    MethodMethod = 0x00,
    MethodSignal = 0x04,
    MethodSlot = 0x08,
    MethodConstructor = 0x0c,
    MethodTypeMask = 0x0c,

    MethodCompatibility = 0x10,
    MethodCloned = 0x20,
    MethodScriptable = 0x40,
    MethodRevisioned = 0x80
};

}

class MetaObject::StringPool
{
public:
    int intern(const QByteArray &str) {
        int size = mStrings.size();
        for (int i = 0; i < size; ++i) {
            if (mStrings[i] == str)
                return i;
        }
        mStrings << str;
        return size;
    }

    QVector<uint8_t> toMetaStringData()
    {
        int count = mStrings.size();
        int size = sizeof(QByteArrayData) * count;
        for (const auto &str : mStrings) {
            size += str.size() + 1;
        }
        QVector<uint8_t> data(size);
        auto arrayDatas = reinterpret_cast<QArrayData *>(data.data());
        auto stringData = reinterpret_cast<char *>(data.data() + sizeof(QByteArrayData) * count);

        int stringOffset = 0;

        for (int i = 0; i < count; ++i) {
            const auto &string = mStrings[i];
            auto size = string.size();

            // write array data
            auto arrayDataOffset = stringOffset + sizeof(QByteArrayData) * (count - i);
            arrayDatas[i] = QArrayData { Q_REFCOUNT_INITIALIZE_STATIC, size, 0, 0, qptrdiff(arrayDataOffset) };

            // write string data
            strcpy(stringData + stringOffset, string.data());
            stringOffset += string.size() + 1;
        }

        return data;
    }

private:
    QList<QByteArray> mStrings;
};


MetaObject::MetaObject(const SP<ForeignClass> &klass) :
    mForeignClassWP(klass)
{
    buildData();

    d.superdata = mSuperMetaObject ? mSuperMetaObject.get() : &QObject::staticMetaObject;
    d.stringdata = reinterpret_cast<const QByteArrayData *>(mStringData.data());
    d.data = mData.data();
    d.static_metacall = nullptr;
    d.relatedMetaObjects = nullptr;
    d.extradata = nullptr;
}

int MetaObject::dynamicMetaCall(Object *obj, QMetaObject::Call call, int index, void **argv)
{
    auto klass = mForeignClassWP.lock();

    if (mSuperMetaObject) {
        index = mSuperMetaObject->dynamicMetaCall(obj, call, index, argv);
    } else {
        index = obj->QObject::qt_metacall(call, index, argv);
    }

    if (index < 0) {
        return index;
    }

    switch (call) {
    case QMetaObject::InvokeMetaMethod: {
        if (index < mMethodIds.size()) {
            if (index < mSignalCount) {
                QMetaObject::activate(obj, this, index, argv);
            } else {
                QVariantList args;
                std::transform(argv + 1, argv + 1 + mMethodArities[index], std::back_inserter(args), [](void *arg) {
                    return *reinterpret_cast<QVariant *>(arg);
                });
                auto result = klass->callMethod(obj, mMethodIds[index], args);
                if (argv[0]) {
                    *static_cast<QVariant *>(argv[0]) = result;
                }
            }
        }
        index -= mMethodIds.size();
    } break;

    case QMetaObject::ReadProperty: {
        if (index < mPropertyIds.size()) {
            auto result = klass->getProperty(obj, mPropertyIds[index]);
            *static_cast<QVariant *>(argv[0]) = result;
        }
        index -= mPropertyIds.size();
    } break;

    case QMetaObject::WriteProperty: {
        if (index < mPropertyIds.size()) {
            auto variant = *static_cast<QVariant *>(argv[0]);
            klass->setProperty(obj, mPropertyIds[index], variant);
        }
        index -= mPropertyIds.size();
    } break;

    case QMetaObject::ResetProperty:
    case QMetaObject::QueryPropertyDesignable:
    case QMetaObject::QueryPropertyScriptable:
    case QMetaObject::QueryPropertyStored:
    case QMetaObject::QueryPropertyEditable:
    case QMetaObject::QueryPropertyUser: {
        index -= mPropertyIds.size();
    } break;

    case QMetaObject::RegisterPropertyMetaType: {
        if (index < mPropertyIds.size()) {
            *static_cast<int *>(argv[0]) = -1;
        }
        index -= mPropertyIds.size();
    } break;

    default: break;
    }

    return index;
}

void MetaObject::buildData()
{
    auto klass = mForeignClassWP.lock();

    int index = 0;
    for (const auto &signal : klass->signalMethods()) {
        mMethodIds << signal.id;
        mMethodArities << signal.arity;
        mSignalIndexHash[signal.id] = index++;
    }
    mSignalCount = klass->signalMethods().size();
    for (const auto &method : klass->nonSignalMethods()) {
        mMethodIds << method.id;
        mMethodArities << method.arity;
    }
    for (const auto &property : klass->properties()) {
        mPropertyIds << property.id;
    }

    StringPool stringPool;
    mData = writeMetaData(stringPool);
    mStringData = stringPool.toMetaStringData();

    if (klass->superClass()) {
        mSuperMetaObject = klass->superClass()->metaObject();
    }
}

QVector<uint> MetaObject::writeMetaData(StringPool &stringPool)
{
    auto klass = mForeignClassWP.lock();

    int methodDataSize = 0;

    for (const auto &method : klass->methods()) {
        methodDataSize += 5 + 1 + method.arity * 2;
    }

    QVector<uint> metaData;

    auto markIndex = [&]() {
        auto index = metaData.size();
        metaData << 0;
        return index;
    };

    auto writeCurrentPos = [&](int markedIndex) {
        metaData[markedIndex] = metaData.size();
    };

    // write header //

    metaData << 7; // revision
    metaData << stringPool.intern(klass->className()); // classname
    metaData << 0 << 0; // classinfo

    auto methods = klass->methods();
    auto signalMethods = klass->signalMethods();
    auto nonSignalMethods = klass->nonSignalMethods();

    // methods
    metaData << (methods.size());
    int methodInfoPosIndex = markIndex();

    auto properties = klass->properties();

    // properties
    metaData << properties.size();
    int propertyInfoPosIndex = markIndex();

    metaData << 0 << 0; // enums
    metaData << 0 << 0; // constructors
    metaData << 0; // flags
    metaData << signalMethods.size(); // signal count

    // write method info //

    writeCurrentPos(methodInfoPosIndex);

    QQueue<int> parameterInfoPosIndexes;

    auto addMethodInfo = [&](const ForeignClass::Method &method) {
        metaData << stringPool.intern(method.name); // name
        metaData << method.arity; // argc
        parameterInfoPosIndexes.enqueue(markIndex()); // parameters
        metaData << 2; // tag
        int flags = AccessPublic;
        if (method.type == ForeignClass::Method::Type::Signal) {
            flags |= AccessProtected;
            flags |= MethodSignal;
        }
        metaData << flags; // flags
    };

    for (const auto &signal : signalMethods) {
        addMethodInfo(signal);
    }
    for (const auto &method : nonSignalMethods) {
        addMethodInfo(method);
    }

    auto addParametersInfo = [&](const ForeignClass::Method &method) {
        writeCurrentPos(parameterInfoPosIndexes.dequeue());
        metaData << QMetaType::QVariant;
        for (int i = 0; i < method.arity; ++i) {
            metaData << QMetaType::QVariant;
        }
        for (int i = 0; i < method.arity; ++i) {
            metaData << stringPool.intern("");
        }
    };

    for (const auto &signal : signalMethods) {
        addParametersInfo(signal);
    }
    for (const auto &method : nonSignalMethods) {
        addParametersInfo(method);
    }

    // write property info //

    writeCurrentPos(propertyInfoPosIndex);

    for (const auto &property : properties) {
        metaData << stringPool.intern(property.name);
        metaData << QMetaType::QVariant;
        int flags = Notify | ResolveEditable | Stored | Scriptable | Designable;
        if (property.flags & ForeignClass::Property::Flag::Writable) {
            flags |= Writable;
        }
        if (property.flags & ForeignClass::Property::Flag::Readable) {
            flags |= Readable;
        }
        if (property.flags & ForeignClass::Property::Flag::Constant) {
            flags |= Constant;
        }
        metaData << flags;
    }

    for (const auto &property : properties) {
        int signalIndex;
        if (property.hasNotifySignal) {
            signalIndex = mSignalIndexHash[property.notifySignalId];
        } else {
            signalIndex = -1;
        }
        metaData << signalIndex;
    }

    // end of data //

    metaData << 0;

    return metaData;
}

} // namespace ForeignQObject
