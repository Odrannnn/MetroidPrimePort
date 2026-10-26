// The Remastered model converter's skinning (weights from the nearest retail vertex) and the
// CSKR and surface header writers. See port_remastered_convert_internal.h.

#include "port_remastered_convert_internal.h"

namespace PortRemastered {
namespace ConvertDetail {

// The column given to each row of an n x m cost matrix (n <= m, row-major)
// that minimises the total cost, each column used at most once (Hungarian).
static std::vector<size_t> AssignRows(const std::vector<double>& cost, size_t n, size_t m) {
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<double> u(n + 1, 0.0), v(m + 1, 0.0);
  std::vector<size_t> p(m + 1, 0), way(m + 1, 0);
  for (size_t i = 1; i <= n; ++i) {
    p[0] = i;
    size_t j0 = 0;
    std::vector<double> minv(m + 1, inf);
    std::vector<bool> used(m + 1, false);
    do {
      used[j0] = true;
      const size_t i0 = p[j0];
      double delta = inf;
      size_t j1 = 0;
      for (size_t j = 1; j <= m; ++j) {
        if (used[j]) {
          continue;
        }
        const double cur = cost[(i0 - 1) * m + (j - 1)] - u[i0] - v[j];
        if (cur < minv[j]) {
          minv[j] = cur;
          way[j] = j0;
        }
        if (minv[j] < delta) {
          delta = minv[j];
          j1 = j;
        }
      }
      for (size_t j = 0; j <= m; ++j) {
        if (used[j]) {
          u[p[j]] += delta;
          v[j] -= delta;
        } else {
          minv[j] -= delta;
        }
      }
      j0 = j1;
    } while (p[j0] != 0);
    do {
      const size_t j1 = way[j0];
      p[j0] = p[j1];
      j0 = j1;
    } while (j0 != 0);
  }
  std::vector<size_t> col(n, 0);
  for (size_t j = 1; j <= m; ++j) {
    if (p[j] != 0) {
      col[p[j] - 1] = j - 1;
    }
  }
  return col;
}

// One weight per vertex and bone, from the Remastered joints or, for a static
// Remastered model, from the nearest retail vertex.
//
// Each vertex votes its weight on a joint for the bones of the nearest retail
// vertex. A joint takes the bone with the most votes, plus any other with a
// kJointSplit share: several joints sharing a bone is the normal case, and a
// joint covering two retail bones is split between them per vertex, so the
// seam falls where retail has it.
std::vector<WeightKey> SkinWeights(const std::vector<double>& P, size_t n, const std::vector<uint16_t>* J,
                                   const std::vector<float>* W, const Retail& retail, Span cskr,
                                   const std::function<void(const std::string&)>& log,
                                   const std::vector<std::array<double, 3>>* pivots,
                                   const std::vector<ConvertOptions::RetailBone>* skeleton,
                                   const std::vector<uint32_t>* tris) {
  const std::vector<SkinGroup> groups = ReadCskr(cskr);
  std::vector<uint32_t> gid;
  for (size_t i = 0; i < groups.size(); ++i) {
    gid.insert(gid.end(), groups[i].count, uint32_t(i));
  }
  const size_t nr = std::min(gid.size(), retail.Count());
  if (nr == 0) {
    throw Fail{"the retail skin covers no vertices"};
  }
  const double* rp = retail.P.data();
  std::vector<uint32_t> nn;
  Nearest(P.data(), n, rp, nr, nn);
  std::vector<std::vector<std::pair<uint32_t, double>>> weights(n);
  if (J && W) {
    std::vector<uint32_t> bones;
    for (const SkinGroup& g : groups) {
      for (const auto& w : g.weights) {
        bones.push_back(w.first);
      }
    }
    std::sort(bones.begin(), bones.end());
    bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
    const size_t nb = bones.size();
    // The retail vertices' weights, a row per vertex and a column per bone.
    std::vector<double> GW(groups.size() * nb, 0.0);
    for (size_t i = 0; i < groups.size(); ++i) {
      for (const auto& w : groups[i].weights) {
        const size_t col = size_t(std::lower_bound(bones.begin(), bones.end(), w.first) - bones.begin());
        GW[i * nb + col] += double(w.second);
      }
    }
    auto RW = [&](size_t v) { return &GW[size_t(gid[v]) * nb]; };
    uint32_t maxJoint = 0;
    for (size_t i = 0; i < n * 4; ++i) {
      maxJoint = std::max<uint32_t>(maxJoint, (*J)[i]);
    }
    const size_t nj = size_t(maxJoint) + 1;
    std::vector<double> V(nj * nb, 0.0);
    for (int k = 0; k < 4; ++k) {
      for (size_t v = 0; v < n; ++v) {
        const double w = double((*W)[v * 4 + k]);
        const double* row = RW(nn[v]);
        double* dst = &V[size_t((*J)[v * 4 + k]) * nb];
        for (size_t c = 0; c < nb; ++c) {
          dst[c] += w * row[c];
        }
      }
    }
    std::vector<std::vector<uint32_t>> assign(nj);
    size_t mapped = 0, split = 0;
    std::vector<double> share(nb);
    for (size_t j = 0; j < nj; ++j) {
      const double total = RowSum(&V[j * nb], nb);
      if (!(total > 0.0)) {
        continue;
      }
      for (size_t c = 0; c < nb; ++c) {
        share[c] = V[j * nb + c] / total;
      }
      std::vector<uint32_t> order(nb);
      for (size_t c = 0; c < nb; ++c) {
        order[c] = uint32_t(c);
      }
      std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return share[a] > share[b]; });
      for (uint32_t c : order) {
        if (share[c] >= kJointSplit) {
          assign[j].push_back(c);
        }
      }
      if (assign[j].empty()) {
        assign[j].push_back(order[0]);
      }
      ++mapped;
    }
    // A joint whose pivot sits on one retail bone's head is that bone, and the
    // vote's share for the bone's ancestors only says the retail skin blends
    // there: split onto them, the joint's vertices stay with the parent as the
    // bone turns (the Parasite Queen's jaw on Jaw_1 and Head_1 tore the mouth
    // open). Those ancestors leave the joint's bones. "Sits on" is a nearest
    // head at most kPivotMatch of the second nearest's distance; a joint
    // between two heads (a collar, a leaf's tip) keeps its vote.
    size_t pivoted = 0;
    if (pivots && skeleton && !skeleton->empty()) {
      constexpr double kPivotMatch = 0.55;
      std::map<uint32_t, size_t> boneOf;
      for (size_t i = 0; i < skeleton->size(); ++i) {
        boneOf.emplace((*skeleton)[i].id, i);
      }
      for (size_t j = 0; j < nj && j < pivots->size(); ++j) {
        if (assign[j].size() < 2) {
          continue;
        }
        const double* p = (*pivots)[j].data();
        double d1 = -1.0, d2 = -1.0;
        size_t nearestCol = 0;
        for (size_t c = 0; c < nb; ++c) {
          const auto it = boneOf.find(bones[c]);
          if (it == boneOf.end()) {
            continue;
          }
          const double* h = (*skeleton)[it->second].head;
          const double d = std::sqrt((p[0] - h[0]) * (p[0] - h[0]) + (p[1] - h[1]) * (p[1] - h[1]) +
                                     (p[2] - h[2]) * (p[2] - h[2]));
          if (d1 < 0.0 || d < d1) {
            d2 = d1;
            d1 = d;
            nearestCol = c;
          } else if (d2 < 0.0 || d < d2) {
            d2 = d;
          }
        }
        if (d2 <= 0.0 || d1 > kPivotMatch * d2 ||
            std::find(assign[j].begin(), assign[j].end(), uint32_t(nearestCol)) == assign[j].end()) {
          continue;
        }
        std::vector<uint32_t> ancestors;
        auto it = boneOf.find(bones[nearestCol]);
        for (size_t guard = 0; it != boneOf.end() && guard < skeleton->size(); ++guard) {
          it = boneOf.find((*skeleton)[it->second].parent);
          if (it != boneOf.end()) {
            ancestors.push_back((*skeleton)[it->second].id);
          }
        }
        const size_t before = assign[j].size();
        assign[j].erase(std::remove_if(assign[j].begin(), assign[j].end(),
                                       [&](uint32_t c) {
                                         return std::find(ancestors.begin(), ancestors.end(), bones[c]) !=
                                                ancestors.end();
                                       }),
                        assign[j].end());
        pivoted += assign[j].size() != before;
      }
    }
    // A rig cut finer than retail's, both of rigid pieces (the iris doors: each
    // of six leaves is a stack of layers and plates there, one piece here): a
    // piece split between bones tears, and the vote is no guide, since retail's
    // few vertices on a wide leaf are nearer the next leaf's edge than its own.
    // Such a piece rides the bone whose piece is nearest its own, whole.
    //
    // A rig as fine as retail's (the Frigate's big doors: two rings of ten
    // pieces each side) has the same tear wherever the vote splits a rigid
    // piece, but nearest alone can send two pieces to one bone and leave
    // another bone, and what it moves, empty. There every bone takes the piece
    // that the cheapest one-to-one pairing of centres gives it, and any piece
    // left over the nearest bone.
    size_t rehomed = 0;
    bool paired = false;
    const bool rigidRetail = std::all_of(groups.begin(), groups.end(),
                                         [](const SkinGroup& g) { return g.weights.size() == 1; });
    if (rigidRetail && mapped * 10 >= nb * 9) {
      std::vector<double> bc(nb * 3, 0.0), bn(nb, 0.0), jc(nj * 3, 0.0), jn(nj, 0.0);
      std::vector<bool> loose(nj, false);
      for (size_t v = 0; v < nr; ++v) {
        const double* row = RW(v);
        const size_t c = size_t(std::max_element(row, row + nb) - row);
        for (int a = 0; a < 3; ++a) {
          bc[c * 3 + a] += rp[v * 3 + a];
        }
        bn[c] += 1.0;
      }
      for (size_t v = 0; v < n; ++v) {
        for (int k = 0; k < 4; ++k) {
          const double w = double((*W)[v * 4 + k]);
          const size_t j = (*J)[v * 4 + k];
          if (w >= 0.999) {
            for (int a = 0; a < 3; ++a) {
              jc[j * 3 + a] += P[v * 3 + a];
            }
            jn[j] += 1.0;
          } else if (w >= 1e-3) {
            loose[j] = true;
          }
        }
      }
      auto dist = [&](size_t j, size_t c) {
        double d = 0.0;
        for (int a = 0; a < 3; ++a) {
          const double e = jc[j * 3 + a] / jn[j] - bc[c * 3 + a] / bn[c];
          d += e * e;
        }
        return d;
      };
      auto nearest = [&](size_t j) {
        double best = -1.0;
        for (size_t c = 0; c < nb; ++c) {
          if (bn[c] == 0.0) {
            continue;
          }
          const double d = dist(j, c);
          if (best < 0.0 || d < best) {
            assign[j] = {uint32_t(c)};
            best = d;
          }
        }
      };
      if (mapped >= 2 * nb) {
        for (size_t j = 0; j < nj; ++j) {
          if (assign[j].empty() || loose[j] || jn[j] == 0.0) {
            continue;
          }
          nearest(j);
          ++rehomed;
        }
      } else {
        std::vector<size_t> pieces;
        bool rigid = true, torn = false;
        for (size_t j = 0; j < nj; ++j) {
          if (assign[j].empty()) {
            continue;
          }
          rigid = rigid && !loose[j] && jn[j] > 0.0;
          torn = torn || assign[j].size() > 1;
          pieces.push_back(j);
        }
        const bool everyBone = std::all_of(bn.begin(), bn.end(), [](double c) { return c > 0.0; });
        const size_t m = pieces.size();
        if (rigid && torn && everyBone && m >= nb) {
          std::vector<double> cost(nb * m);
          for (size_t c = 0; c < nb; ++c) {
            for (size_t i = 0; i < m; ++i) {
              cost[c * m + i] = dist(pieces[i], c);
            }
          }
          const std::vector<size_t> col = AssignRows(cost, nb, m);
          std::vector<bool> taken(m, false);
          for (size_t c = 0; c < nb; ++c) {
            assign[pieces[col[c]]] = {uint32_t(c)};
            taken[col[c]] = true;
          }
          for (size_t i = 0; i < m; ++i) {
            if (!taken[i]) {
              nearest(pieces[i]);
            }
          }
          rehomed = m;
          paired = true;
        } else if (torn && everyBone && m <= nb &&
                   std::all_of(pieces.begin(), pieces.end(), [&](size_t j) { return jn[j] > 0.0; })) {
          // A rig as fine as retail's or a piece short, not quite rigid (the
          // arm cannon: 34 or 35 joints on 35 bones, the four muzzle petals
          // among them, split between each other by the vote, so the open
          // petals tore into a box). Each joint takes its own bone, and the
          // few vertices blended between joints stay blended, between their
          // bones. The pairing maximises the vote, not closeness: the gun's
          // stack of three rings has a bone between them, and by centres
          // alone every ring slid a step onto its neighbour's bone.
          std::vector<double> cost(m * nb);
          for (size_t i = 0; i < m; ++i) {
            const double* row = &V[pieces[i] * nb];
            const double total = RowSum(row, nb);
            for (size_t c = 0; c < nb; ++c) {
              cost[i * nb + c] = 1.0 - row[c] / total;
            }
          }
          const std::vector<size_t> col = AssignRows(cost, m, nb);
          for (size_t i = 0; i < m; ++i) {
            assign[pieces[i]] = {uint32_t(col[i])};
          }
          rehomed = m;
          paired = true;
        }
      }
    }
    for (size_t j = 0; j < nj; ++j) {
      split += assign[j].size() > 1;
    }
    // MP_REMASTERED_JOINTS=1: each joint's vote and the bones it got, with the
    // centres of its vertices and of each retail bone's.
    if (port::EnvFlag("MP_REMASTERED_JOINTS")) {
      char buf[256];
      std::vector<double> bc(nb * 3, 0.0), bn(nb, 0.0), jc(nj * 3, 0.0), jn(nj, 0.0);
      for (size_t v = 0; v < nr; ++v) {
        const double* row = RW(v);
        const size_t c = size_t(std::max_element(row, row + nb) - row);
        for (int a = 0; a < 3; ++a) {
          bc[c * 3 + a] += rp[v * 3 + a];
        }
        bn[c] += 1.0;
      }
      for (size_t v = 0; v < n; ++v) {
        const size_t k = size_t(std::max_element(W->begin() + v * 4, W->begin() + v * 4 + 4) - (W->begin() + v * 4));
        const size_t j = (*J)[v * 4 + k];
        for (int a = 0; a < 3; ++a) {
          jc[j * 3 + a] += P[v * 3 + a];
        }
        jn[j] += 1.0;
      }
      size_t looseVerts = 0, multiGroups = 0;
      for (size_t v = 0; v < n * 4; ++v) {
        looseVerts += (*W)[v] >= 1e-3 && (*W)[v] < 0.999;
      }
      for (const SkinGroup& g : groups) {
        multiGroups += g.weights.size() > 1;
      }
      log("  rigid retail " + std::to_string(rigidRetail) + " (" + std::to_string(multiGroups) + " of " +
          std::to_string(groups.size()) + " groups blended), " + std::to_string(looseVerts) + " partial weights of " +
          std::to_string(n));
      for (size_t c = 0; c < nb; ++c) {
        const double d = std::max(bn[c], 1.0);
        std::snprintf(buf, sizeof(buf), "  bone %u: %d verts at (%.3f %.3f %.3f)", bones[c], int(bn[c]),
                      bc[c * 3] / d, bc[c * 3 + 1] / d, bc[c * 3 + 2] / d);
        log(buf);
      }
      for (size_t j = 0; j < nj; ++j) {
        const double total = RowSum(&V[j * nb], nb);
        if (!(total > 0.0)) {
          continue;
        }
        const double d = std::max(jn[j], 1.0);
        std::string line;
        std::snprintf(buf, sizeof(buf), "  joint %zu: %d verts at (%.3f %.3f %.3f) ->", j, int(jn[j]), jc[j * 3] / d,
                      jc[j * 3 + 1] / d, jc[j * 3 + 2] / d);
        line = buf;
        for (uint32_t c : assign[j]) {
          std::snprintf(buf, sizeof(buf), " %u(%.2f)", bones[c], V[j * nb + c] / total);
          line += buf;
        }
        line += ", vote";
        std::vector<uint32_t> order(nb);
        for (size_t c = 0; c < nb; ++c) {
          order[c] = uint32_t(c);
        }
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return V[j * nb + a] > V[j * nb + b]; });
        for (size_t i = 0; i < std::min<size_t>(3, nb) && V[j * nb + order[i]] > 0.0; ++i) {
          std::snprintf(buf, sizeof(buf), " %u(%.2f)", bones[order[i]], V[j * nb + order[i]] / total);
          line += buf;
        }
        log(line);
      }
    }
    log("  joints mapped " + std::to_string(mapped) + ", " + std::to_string(split) + " split" +
        (pivoted ? ", " + std::to_string(pivoted) + " off their bone's ancestors" : std::string()) +
        (rehomed ? ", " + std::to_string(rehomed) + (paired ? " rigid paired by piece" : " rigid by nearest piece")
                 : std::string()));
    std::vector<double> VW(n * nb, 0.0);
    std::vector<uint32_t> sel, on, near;
    std::vector<double> selP, onP;
    for (size_t j = 0; j < nj; ++j) {
      const std::vector<uint32_t>& cs = assign[j];
      if (cs.empty()) {
        continue;
      }
      for (int k = 0; k < 4; ++k) {
        sel.clear();
        for (size_t v = 0; v < n; ++v) {
          if ((*J)[v * 4 + k] == j && double((*W)[v * 4 + k]) >= 1e-3) {
            sel.push_back(uint32_t(v));
          }
        }
        if (sel.empty()) {
          continue;
        }
        if (cs.size() == 1) {
          for (uint32_t v : sel) {
            VW[size_t(v) * nb + cs[0]] += double((*W)[size_t(v) * 4 + k]);
          }
          continue;
        }
        // A split joint hands each vertex the weights of the nearest retail
        // vertex that is on one of the joint's bones, restricted to those
        // bones, so a vertex can never leave the joint's own bones.
        on.clear();
        onP.clear();
        std::vector<double> part(cs.size());
        for (size_t r = 0; r < nr; ++r) {
          for (size_t c = 0; c < cs.size(); ++c) {
            part[c] = RW(r)[cs[c]];
          }
          if (RowSum(part.data(), part.size()) > 0.0) {
            on.push_back(uint32_t(r));
            onP.insert(onP.end(), rp + 3 * r, rp + 3 * r + 3);
          }
        }
        selP.clear();
        for (uint32_t v : sel) {
          selP.insert(selP.end(), P.begin() + 3 * size_t(v), P.begin() + 3 * size_t(v) + 3);
        }
        Nearest(selP.data(), sel.size(), onP.data(), on.size(), near);
        for (size_t i = 0; i < sel.size(); ++i) {
          const double* row = RW(on[near[i]]);
          for (size_t c = 0; c < cs.size(); ++c) {
            part[c] = row[cs[c]];
          }
          const double total = RowSum(part.data(), part.size());
          const double w = double((*W)[size_t(sel[i]) * 4 + k]);
          for (size_t c = 0; c < cs.size(); ++c) {
            VW[size_t(sel[i]) * nb + cs[c]] += w * part[c] / total;
          }
        }
      }
    }
    size_t lost = 0;
    for (size_t v = 0; v < n; ++v) {
      double* row = &VW[v * nb];
      if (RowSum(row, nb) <= 0.0) {
        // No weighted joint of this vertex has a bone: take the nearest retail
        // vertex's weights rather than pin it to an arbitrary bone.
        std::copy(RW(nn[v]), RW(nn[v]) + nb, row);
        ++lost;
      }
      for (size_t c = 0; c < nb; ++c) {
        if (row[c] > 0.0) {
          weights[v].emplace_back(bones[c], row[c]);
        }
      }
    }
    if (lost) {
      log("  " + std::to_string(lost) + " vertices with no mapped joint: weights from the nearest retail vertex");
    }
  } else {
    log("  static Remastered model on a skinned retail one: weights from the nearest retail vertex");
    // The group each vertex takes. Where retail binds every vertex of a
    // connected Remastered piece rigidly, the piece takes the group most of its
    // vertices are nearest to: per vertex, a piece lying across two retail
    // bones (the gunship's hatch rim, its iris petals) tears when they move.
    std::vector<uint32_t> take(n);
    for (size_t v = 0; v < n; ++v) {
      take[v] = gid[nn[v]];
    }
    if (tris) {
      std::vector<uint32_t> root(n);
      for (size_t v = 0; v < n; ++v) {
        root[v] = uint32_t(v);
      }
      auto find = [&](uint32_t v) {
        while (root[v] != v) {
          v = root[v] = root[root[v]];
        }
        return v;
      };
      for (size_t t = 0; t + 2 < tris->size(); t += 3) {
        for (int k = 1; k < 3; ++k) {
          const uint32_t a = find((*tris)[t]), b = find((*tris)[t + k]);
          if (a != b) {
            root[b] = a;
          }
        }
      }
      std::map<uint32_t, std::map<uint32_t, size_t>> votes;  // piece -> group -> vertices
      std::vector<char> smooth(n, 0);                          // per piece root: some vertex is blended
      for (size_t v = 0; v < n; ++v) {
        const uint32_t r = find(uint32_t(v));
        ++votes[r][take[v]];
        smooth[r] |= groups[take[v]].weights.size() != 1;
      }
      size_t moved = 0;
      std::map<uint32_t, uint32_t> pick;
      for (const auto& [r, by] : votes) {
        if (by.size() < 2 || smooth[r]) {
          continue;
        }
        // Most votes; a tie goes to the lower group, so the result is stable.
        pick[r] = std::max_element(by.begin(), by.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
      }
      for (size_t v = 0; v < n; ++v) {
        const auto it = pick.find(find(uint32_t(v)));
        if (it != pick.end() && take[v] != it->second) {
          take[v] = it->second;
          ++moved;
        }
      }
      if (moved) {
        log("  " + std::to_string(pick.size()) + " rigid pieces across retail bones: " + std::to_string(moved) +
            " vertices moved to their piece's bone");
      }
    }
    for (size_t v = 0; v < n; ++v) {
      for (const auto& w : groups[take[v]].weights) {
        bool found = false;
        for (auto& have : weights[v]) {
          if (have.first == w.first) {
            have.second = double(w.second);
            found = true;
          }
        }
        if (!found) {
          weights[v].emplace_back(w.first, double(w.second));
        }
      }
    }
  }
  return QuantiseWeights(weights);
}

std::vector<WeightKey> QuantiseWeights(std::vector<std::vector<std::pair<uint32_t, double>>>& weights) {
  // Quantise to 1/64 and renormalise, so vertices that differ by noise share a
  // weight group.
  const size_t n = weights.size();
  std::vector<WeightKey> out(n);
  for (size_t v = 0; v < n; ++v) {
    auto& ws = weights[v];
    if (ws.size() > kMaxSkinWeights) {
      std::stable_sort(ws.begin(), ws.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      ws.resize(kMaxSkinWeights);
    }
    double total = 0.0;
    for (const auto& w : ws) {
      total += w.second;
    }
    WeightKey q;
    if (total <= 0.0) {
      if (!ws.empty()) {
        q.emplace_back(ws[0].first, 1.0);  // no usable weights: the strongest (first) bone alone
      }
      out[v] = std::move(q);
      continue;
    }
    for (const auto& w : ws) {
      const double r = std::nearbyint(w.second / total * 64.0);
      if (r != 0.0) {
        q.emplace_back(w.first, r / 64.0);
      }
    }
    std::sort(q.begin(), q.end());
    double s = 0.0;
    for (const auto& w : q) {
      s += w.second;
    }
    if (s > 0.0) {
      for (auto& w : q) {
        w.second /= s;
      }
    }
    out[v] = std::move(q);
  }
  return out;
}

Blob CskrBytes(const std::vector<std::pair<const WeightKey*, uint32_t>>& runs, size_t total) {
  Blob b;
  P32(b, uint32_t(runs.size()));
  for (const auto& run : runs) {
    P32(b, uint32_t(run.first->size()));
    for (const auto& w : *run.first) {
      P32(b, w.first);
      PF(b, w.second);
    }
    P32(b, run.second);
  }
  P32(b, 0xFFFFFFFFu);
  P32(b, uint32_t(total));
  P32(b, 0xFFFFFFFFu);
  P32(b, uint32_t(total));
  return b;
}

void SurfaceHeader(Blob& s, const double* centre, uint32_t mat, size_t dlSize, uint32_t extra) {
  for (int c = 0; c < 3; ++c) {
    PF(s, centre[c]);
  }
  P32(s, mat);
  P32(s, uint32_t(dlSize) | 0x80000000u);
  P32(s, 0);
  P32(s, 0);
  P32(s, extra);
  P32(s, 0);
  P32(s, 0);
  PF(s, 1.0);
}

}  // namespace ConvertDetail

}  // namespace PortRemastered
