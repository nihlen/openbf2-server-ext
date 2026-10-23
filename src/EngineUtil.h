// Engine helpers shared by the bot channel (Module.cpp) and the admin channel (Admin.cpp). Inline
// header code and typed virtual calls only, because bf2 exports no symbols to modules.

#ifndef SERVEREXT_ENGINEUTIL_H
#define SERVEREXT_ENGINEUTIL_H

#include <cmath>
#include <string>

#include "Core/Ids.h"
#include "Game/Objects/GameLogicComp/IArmorGLComp.h"
#include "Game/Objects/ICameraObject.h"
#include "Game/Objects/IObject.h"
#include "Game/Objects/IObjectTemplate.h"
#include "Game/Objects/IPlayer.h"
#include "Game/Objects/IPlayerControlObjectTemplate.h"
#include "IO/Network/INetworkable.h"
#include "Scene/Object/ObjectTypes/Game/IPlayerControlObject.h"

namespace serverext {
namespace engine {

using namespace dice::hfe;
using namespace dice::hfe::world;

inline IObject* rootOf(IObject* o)
{
    if (!o)
        return 0;
    while (o->getParent())
        o = o->getParent();
    return o;
}

inline std::string templateName(IObject* o)
{
    return o && o->getTemplate() ? o->getTemplate()->getName() : std::string();
}

// The root's PlayerControlObject template interface (vehicle category and type), or null.
inline IPlayerControlObjectTemplate* pcoTemplateOf(IObject* o)
{
    const IObjectTemplate* t = o ? o->getTemplate() : 0;
    return t ? static_cast<IPlayerControlObjectTemplate*>(
                   const_cast<IObjectTemplate*>(t)->queryInterface(IID_IPlayerControlObjectTemplate))
             : 0;
}

inline uint32_t netIdOf(IObject* o)
{
    io::INetworkable* net = o ? o->getNetworkable() : 0;
    return net ? net->m_networkableId : 0;
}

// A vehicle in protocol.md's sense: a root object with seats (IPlayerControlObject) that is not a
// soldier. Cameras are excluded too: every player's MultiPlayerFreeCamera is a root
// IPlayerControlObject that its player occupies while dead or spectating.
inline IPlayerControlObject* vehiclePco(IObject* o)
{
    if (!o || o->getParent() || o->queryInterface(IID_ISoldier) || o->queryInterface(IID_ICameraObject))
        return 0;
    return static_cast<IPlayerControlObject*>(o->queryInterface(IID_IPlayerControlObject));
}

// The vehicle seat a player occupies, or null when on foot, dead or in a non-vehicle object.
inline IPlayerControlObject* seatOf(world::IPlayer* p, IObject** rootOut)
{
    IObject* vehicle = p->getVehicle();
    IObject* root = rootOf(vehicle);
    if (rootOut)
        *rootOut = root;
    if (!p->getIsAlive() || !vehicle || !vehiclePco(root))
        return 0;
    return static_cast<IPlayerControlObject*>(vehicle->queryInterface(IID_IPlayerControlObject));
}

// Health of a vehicle root as 0..1 from its armour component, or -1 without one. destroyed is set
// when the hit points are at or below 0.
inline float healthOf(IObject* root, bool* destroyed)
{
    if (destroyed)
        *destroyed = false;
    IArmorGLComp* armor = static_cast<IArmorGLComp*>(root->queryComponent(IID_IArmorGLComp, IID_IArmorGLComp));
    if (!armor)
        return -1.0f;
    const float hp = armor->getHitPoints();
    if (destroyed)
        *destroyed = !(hp > 0.0f);
    const float maxHp = armor->getMaxHitPoints();
    float h = maxHp > 0.0f ? hp / maxHp : 0.0f;
    if (!(h >= 0.0f)) h = 0.0f;
    if (h > 1.0f) h = 1.0f;
    return h;
}

inline float yawOf(const Mat4& m)
{
    return std::atan2(m.m[8], m.m[10]) * 57.29577951f;
}

// Yaw, pitch and roll in protocol.md's conventions from an absolute transformation.
inline void attitudeOf(const Mat4& m, float ypr[3])
{
    const float r2d = 57.29577951f;
    float s = m.m[9];
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    ypr[0] = std::atan2(m.m[8], m.m[10]) * r2d;
    ypr[1] = -std::asin(s) * r2d;
    ypr[2] = std::atan2(m.m[1], m.m[5]) * r2d;
}

} // namespace engine
} // namespace serverext

#endif // SERVEREXT_ENGINEUTIL_H
