// Spawn-group choices for a bot's team (docs/protocol.md "Spawning"). Engine-free so the
// unit tests can drive it: G is IServerExtension::SpawnGroupInfo in the module, or any struct with
// the same fields (id, squad, pointCount, enterOnSpawnPoints, active, onlyForHuman, x, y, z).

#ifndef SERVEREXT_SPAWNPLAN_H
#define SERVEREXT_SPAWNPLAN_H

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace serverext {
namespace spawn {

template <class G>
bool usable(const G& g)
{
    return g.squad <= 0 && !g.onlyForHuman && g.pointCount > 0;
}

// The pad: the team's lowest-id usable group without enter-on-spawn points, active or not.
// Matches findSpawnGroup(team, false) whenever nothing is blocked.
template <class G>
int homeGroup(const G* groups, int n)
{
    for (int i = 0; i < n; ++i)
        if (usable(groups[i]) && groups[i].enterOnSpawnPoints == 0)
            return groups[i].id;
    return -1;
}

template <class G>
int firstUsableGroup(const G* groups, int n)
{
    for (int i = 0; i < n; ++i)
        if (usable(groups[i]))
            return groups[i].id;
    return -1;
}

template <class G>
const G* findGroup(const G* groups, int n, int id)
{
    for (int i = 0; i < n; ++i)
        if (groups[i].id == id)
            return &groups[i];
    return 0;
}

// The groups a bot of a team starts from: home (-1 without an on-foot group) and the first
// target. ok is false when the team has no usable group at all, CreateBot's Error 6 case.
struct Choice
{
    bool ok;
    int home;
    int target;
};

template <class G>
Choice choose(const G* groups, int n)
{
    Choice c;
    c.home = homeGroup(groups, n);
    c.target = c.home >= 0 ? c.home : firstUsableGroup(groups, n);
    c.ok = c.target >= 0;
    return c;
}

// Home first, then the other on-foot groups nearest to home first, then the groups with any
// enter-on-spawn point, nearest first. The distances are from home, or from target without one.
template <class G>
std::vector<int> fallbackOrder(int home, int target, const G* groups, int n)
{
    const G* ref = findGroup(groups, n, home >= 0 ? home : target);
    std::vector<std::pair<float, int> > onFoot, enters;
    for (int i = 0; i < n; ++i)
    {
        const G& g = groups[i];
        if (!usable(g) || g.id == home)
            continue;
        float d = 0.0f;
        if (ref)
        {
            const float dx = g.x - ref->x, dy = g.y - ref->y, dz = g.z - ref->z;
            d = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        (g.enterOnSpawnPoints > 0 ? enters : onFoot).push_back(std::make_pair(d, g.id));
    }
    std::stable_sort(onFoot.begin(), onFoot.end());
    std::stable_sort(enters.begin(), enters.end());
    std::vector<int> order;
    if (home >= 0)
        order.push_back(home);
    for (size_t i = 0; i < onFoot.size(); ++i)
        order.push_back(onFoot[i].second);
    for (size_t i = 0; i < enters.size(); ++i)
        order.push_back(enters[i].second);
    return order;
}

} // namespace spawn
} // namespace serverext

#endif
