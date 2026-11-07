#include "port_remastered_map.h"
#include "port_bytes.h"

#include "port_map_icons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace PortRemastered {
namespace {

// The disc's icons are 64x64; Remastered's are 36x36.
constexpr int kIconSize = 64;

// CMappableObject's types for the arrows (port_map_icons.h).
constexpr int kDownYellow = 27;
constexpr int kUpYellow = 28;
constexpr int kDownGreen = 29;
constexpr int kUpGreen = 30;
constexpr int kDownRed = 31;
constexpr int kUpRed = 32;

// --- Room maps ----------------------------------------------------------------

constexpr uint32_t kMLVL = 0x4D4C564C;
constexpr uint32_t kMAPW = 0x4D415057;
constexpr uint32_t kMAPA = 0x4D415041;
constexpr size_t kMapaHeader = 52;
constexpr size_t kMapaObject = 0x50;
constexpr size_t kMapaSurface = 32;
constexpr size_t kMapaMaxVertices = 255;  // a MAPA's indices are bytes
constexpr uint32_t kMaxTriangles = 8192;   // per area; real ones have a few hundred
constexpr uint32_t kGxTriangles = 0x90;

// Corners closer than this are one corner.
constexpr double kWeld = 0.01;
// Neighbouring triangles closer in direction than this (cosine; 29 degrees) are
// one face: the disc's faces are not all flat, a curved wall is one face. The
// value gives the disc's face count on the 261 rooms Remastered left alone.
constexpr double kFaceCosine = 0.875;
// A room is the disc's when no corner of either is further than this from a
// corner of the other (sum of the three axes). Unchanged rooms are within 0.1.
constexpr double kSameRoom = 0.5;
// A pairing worse than this (see Pair) is not a pairing; the reshaped rooms
// reach 14.
constexpr double kMaxPairCost = 40.0;
// Remastered draws the levels of a world further apart than they are: the
// Phazon Mines' three, by this much.
constexpr double kLevelGap = 200.0;

using Vec = std::array<double, 3>;
using Mat = std::array<double, 12>;  // 3x4, row major

Vec Sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec Cross(const Vec& a, const Vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec Neg(const Vec& a) { return {-a[0], -a[1], -a[2]}; }
bool Normalize(Vec& a) {
  const double length = std::sqrt(Dot(a, a));
  if (length < 1e-9) {
    return false;
  }
  a = {a[0] / length, a[1] / length, a[2] / length};
  return true;
}

using port::AppendBE32;
using port::ReadBE32;
using port::ReadLE32;
using port::ReadLE64;
float AsFloat(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void PutBeFloat(std::vector<uint8_t>& out, double v) {
  const float f = float(v);
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  AppendBE32(out, bits);
}

struct Box {
  Vec min{};
  Vec max{};
  Vec Extent() const { return Sub(max, min); }
};
Box BoxOf(const std::vector<Vec>& points) {
  Box box{points[0], points[0]};
  for (const Vec& p : points) {
    for (size_t k = 0; k < 3; ++k) {
      box.min[k] = std::min(box.min[k], p[k]);
      box.max[k] = std::max(box.max[k], p[k]);
    }
  }
  return box;
}

// The MLVL's areas' transforms in order, and its MAPW.
bool ReadMlvl(const std::vector<uint8_t>& d, std::vector<Mat>& areas, uint32_t& mapw) {
  size_t o = 0;
  const auto need = [&](size_t n) { return o <= d.size() && n <= d.size() - o; };
  // Skips a count and `unit` bytes per entry.
  const auto skip = [&](size_t unit) {
    if (!need(4)) {
      return false;
    }
    const uint64_t bytes = uint64_t(ReadBE32(&d[o])) * unit;
    o += 4;
    if (bytes > d.size() || !need(size_t(bytes))) {
      return false;
    }
    o += size_t(bytes);
    return true;
  };
  o = 20;
  if (!skip(11) || !need(8)) {
    return false;
  }
  const uint32_t count = ReadBE32(&d[o]);
  o += 8;
  if (count > 4096) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!need(84)) {
      return false;
    }
    Mat xf{};
    for (size_t k = 0; k < 12; ++k) {
      xf[k] = AsFloat(ReadBE32(&d[o + 4 + 4 * k]));
    }
    for (const double f : xf) {
      if (!std::isfinite(f)) {
        return false;
      }
    }
    areas.push_back(xf);
    o += 84;  // name, transform, box, MREA, internal id
    if (!skip(2)) {  // attached areas
      return false;
    }
    o += 4;
    if (!skip(8) || !skip(4) || !need(4)) {  // dependencies, their layer offsets
      return false;
    }
    const uint32_t docks = ReadBE32(&d[o]);
    o += 4;
    for (uint32_t k = 0; k < docks; ++k) {
      if (!skip(8) || !skip(12)) {  // connections, corners
        return false;
      }
    }
  }
  if (!need(4)) {
    return false;
  }
  mapw = ReadBE32(&d[o]);
  return true;
}

// One of the disc's map areas.
struct RetailArea {
  uint32_t mapa = 0;
  Mat xf{};
  std::vector<uint8_t> data;
  size_t objects = 0;
  size_t surfaces = 0;
  std::vector<Vec> vertices;  // in the area
  Box world;                  // of the vertices, in the world
};

bool ReadMapa(RetailArea& area) {
  const std::vector<uint8_t>& d = area.data;
  if (d.size() < kMapaHeader || ReadBE32(&d[0]) != 0xDEADD00D) {
    return false;
  }
  area.objects = ReadBE32(&d[40]);
  const size_t vertices = ReadBE32(&d[44]);
  area.surfaces = ReadBE32(&d[48]);
  if (area.objects > d.size() / kMapaObject || vertices > d.size() / 12 ||
      kMapaHeader + area.objects * kMapaObject + vertices * 12 > d.size()) {
    return false;
  }
  const uint8_t* p = &d[0] + kMapaHeader + area.objects * kMapaObject;
  for (size_t i = 0; i < vertices; ++i, p += 12) {
    area.vertices.push_back({AsFloat(ReadBE32(p)), AsFloat(ReadBE32(p + 4)), AsFloat(ReadBE32(p + 8))});
  }
  return true;
}

Vec ToWorld(const Mat& m, const Vec& p) {
  return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
          m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}
// An area's transform is a rotation and a translation.
Vec ToArea(const Mat& m, const Vec& p) {
  const Vec q = {p[0] - m[3], p[1] - m[7], p[2] - m[11]};
  return {m[0] * q[0] + m[4] * q[1] + m[8] * q[2], m[1] * q[0] + m[5] * q[1] + m[9] * q[2],
          m[2] * q[0] + m[6] * q[1] + m[10] * q[2]};
}

// One of Remastered's map areas, in the disc's axes.
struct RemasteredArea {
  std::string name;
  std::vector<Vec> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  Box box;
};

// A CMAP: a 32 byte header, the world's name, then its areas. An area is a
// guid, a name, an index, a visibility mode, a 3x4 matrix, three floats and the
// size of its chunks (VERT, MTRL, TRIS, TREE); the areas' vertices are already
// in the world. An area without geometry is left out.
bool ReadCmap(const uint8_t* d, size_t size, std::vector<RemasteredArea>& out) {
  size_t o = 32;
  const auto need = [&](uint64_t n) { return o <= size && n <= size - o; };
  const auto readString = [&](std::string* text) {
    if (!need(4)) {
      return false;
    }
    const uint32_t length = ReadLE32(d + o);
    o += 4;
    if (!need(length)) {
      return false;
    }
    if (text != nullptr) {
      text->assign(reinterpret_cast<const char*>(d + o), length);
    }
    o += length;
    return true;
  };
  if (size < 32 || !readString(nullptr) || !need(4)) {
    return false;
  }
  const uint32_t count = ReadLE32(d + o);
  o += 4;
  for (uint32_t i = 0; i < count; ++i) {
    RemasteredArea area;
    if (!need(16)) {
      return false;
    }
    o += 16;
    if (!readString(&area.name) || !need(8 + 48 + 12 + 8)) {
      return false;
    }
    o += 8 + 48 + 12;
    const uint64_t chunks = ReadLE64(d + o);
    o += 8;
    if (!need(chunks)) {
      return false;
    }
    const size_t end = o + size_t(chunks);
    while (end - o >= 24) {
      const uint8_t* tag = d + o;
      const uint64_t length = ReadLE64(d + o + 4);
      o += 24;  // tag, size, version, a zero
      if (length > end - o) {
        return false;
      }
      const uint8_t* body = d + o;
      o += size_t(length);
      if (length < 4) {
        continue;
      }
      const uint32_t n = ReadLE32(body);
      // Only the first of each: a second would number its corners from zero again.
      if (std::memcmp(tag, "VERT", 4) == 0 && area.vertices.empty() && uint64_t(n) * 12 <= length - 4) {
        for (uint32_t v = 0; v < n; ++v) {
          const uint8_t* p = body + 4 + size_t(v) * 12;
          // Remastered: x to the other side, y up.
          area.vertices.push_back({-double(AsFloat(ReadLE32(p))), AsFloat(ReadLE32(p + 8)), AsFloat(ReadLE32(p + 4))});
        }
      } else if (std::memcmp(tag, "TRIS", 4) == 0 && area.triangles.empty() && n <= kMaxTriangles &&
                 uint64_t(n) * 16 <= length - 4) {
        for (uint32_t t = 0; t < n; ++t) {
          const uint8_t* p = body + 4 + size_t(t) * 16;  // three corners and a zero
          // The other way round, for the turned x.
          area.triangles.push_back({ReadLE32(p), ReadLE32(p + 8), ReadLE32(p + 4)});
        }
      }
    }
    o = end;
    bool valid = !area.vertices.empty() && !area.triangles.empty();
    for (const Vec& v : area.vertices) {
      valid = valid && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }
    for (const auto& t : area.triangles) {
      valid = valid && t[0] < area.vertices.size() && t[1] < area.vertices.size() && t[2] < area.vertices.size();
    }
    if (valid) {
      area.box = BoxOf(area.vertices);
      out.push_back(std::move(area));
    }
  }
  return true;
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  return n % 2 != 0 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

struct Triangle {
  std::array<uint32_t, 3> v{};
  Vec normal{};
  void Flip() {
    std::swap(v[1], v[2]);
    normal = Neg(normal);
  }
};
using Edge = std::pair<uint32_t, uint32_t>;
Edge Undirected(uint32_t a, uint32_t b) { return a < b ? Edge(a, b) : Edge(b, a); }

// A face of a MAPA: what is filled, and the lines drawn around it.
struct Face {
  Vec normal{};
  Vec centre{};
  std::vector<uint8_t> triangles;
  std::vector<std::vector<uint8_t>> outlines;
};

// A triangle soup as the faces of a MAPA. Remastered has neither faces nor
// outlines, and winds its triangles either way, so: triangles that meet at a
// shallow angle are one face, a face's outline is the edges it shares with no
// triangle of its own, and its normal points into the room, as the disc's do
// (the map shades a face by how far it faces the camera).
bool BuildFaces(const std::vector<Vec>& points, const std::vector<std::array<uint32_t, 3>>& soup,
                std::vector<Vec>& vertices, std::vector<Face>& faces) {
  // Weld, drop what collapses, and keep the corners still in use.
  std::map<std::array<long long, 3>, uint32_t> welded;
  std::vector<Vec> all;
  std::vector<uint32_t> remap;
  for (const Vec& p : points) {
    const std::array<long long, 3> key = {std::llround(p[0] / kWeld), std::llround(p[1] / kWeld),
                                          std::llround(p[2] / kWeld)};
    const auto found = welded.emplace(key, uint32_t(all.size()));
    if (found.second) {
      all.push_back(p);
    }
    remap.push_back(found.first->second);
  }
  std::vector<Triangle> triangles;
  std::map<uint32_t, uint32_t> used;
  for (const auto& t : soup) {
    Triangle triangle;
    triangle.v = {remap[t[0]], remap[t[1]], remap[t[2]]};
    if (triangle.v[0] == triangle.v[1] || triangle.v[1] == triangle.v[2] || triangle.v[0] == triangle.v[2]) {
      continue;
    }
    triangle.normal = Cross(Sub(all[triangle.v[1]], all[triangle.v[0]]), Sub(all[triangle.v[2]], all[triangle.v[0]]));
    if (!Normalize(triangle.normal)) {
      continue;
    }
    for (uint32_t v : triangle.v) {
      used.emplace(v, 0);
    }
    triangles.push_back(triangle);
  }
  if (triangles.empty() || used.size() > kMapaMaxVertices) {
    return false;
  }
  for (auto& [index, compact] : used) {
    compact = uint32_t(vertices.size());
    vertices.push_back(all[index]);
  }
  for (Triangle& triangle : triangles) {
    for (uint32_t& v : triangle.v) {
      v = used[v];
    }
  }

  // Faces: triangles joined across the edges where they meet at a shallow angle.
  std::map<Edge, std::vector<size_t>> edges;
  for (size_t i = 0; i < triangles.size(); ++i) {
    for (size_t k = 0; k < 3; ++k) {
      edges[Undirected(triangles[i].v[k], triangles[i].v[(k + 1) % 3])].push_back(i);
    }
  }
  std::vector<size_t> parent(triangles.size());
  for (size_t i = 0; i < parent.size(); ++i) {
    parent[i] = i;
  }
  const auto find = [&](size_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  for (const auto& [edge, list] : edges) {
    for (size_t k = 1; k < list.size(); ++k) {
      if (std::fabs(Dot(triangles[list[0]].normal, triangles[list[k]].normal)) > kFaceCosine) {
        parent[find(list[k])] = find(list[0]);
      }
    }
  }
  std::map<size_t, std::vector<size_t>> groups;
  for (size_t i = 0; i < triangles.size(); ++i) {
    groups[find(i)].push_back(i);
  }

  const auto area2 = [&](const Triangle& t) {
    const Vec c = Cross(Sub(vertices[t.v[1]], vertices[t.v[0]]), Sub(vertices[t.v[2]], vertices[t.v[0]]));
    return Dot(c, c);
  };
  // Wind each piece of the room one way, outwards from its largest triangle,
  // across the edges two triangles share (and within a face, any edge).
  std::vector<bool> seen(triangles.size(), false);
  std::vector<std::vector<size_t>> pieces;
  std::vector<size_t> order(triangles.size());
  for (size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t x, size_t y) { return area2(triangles[x]) > area2(triangles[y]); });
  for (size_t largest : order) {
    if (seen[largest]) {
      continue;
    }
    seen[largest] = true;
    std::vector<size_t> piece;
    std::vector<size_t> todo = {largest};
    while (!todo.empty()) {
      const size_t i = todo.back();
      todo.pop_back();
      piece.push_back(i);
      const std::array<uint32_t, 3> a = triangles[i].v;
      for (size_t k = 0; k < 3; ++k) {
        const uint32_t from = a[k];
        const uint32_t to = a[(k + 1) % 3];
        const std::vector<size_t>& list = edges[Undirected(from, to)];
        for (size_t j : list) {
          if (seen[j] || (list.size() != 2 && find(j) != find(i))) {
            continue;
          }
          seen[j] = true;
          // Neighbours wound alike run along their shared edge in opposite directions.
          const std::array<uint32_t, 3>& b = triangles[j].v;
          for (size_t m = 0; m < 3; ++m) {
            if (b[m] == from && b[(m + 1) % 3] == to) {
              triangles[j].Flip();
              break;
            }
          }
          todo.push_back(j);
        }
      }
    }
    pieces.push_back(std::move(piece));
  }
  // Into the room: a piece faces inwards when it encloses a negative volume
  // around the room's centre. Remastered's rooms are open, so counting the walls
  // a ray crosses from one face says little; the whole piece's volume holds.
  Vec middle{};
  double weights = 0;
  for (const Triangle& t : triangles) {
    const double weight = std::sqrt(area2(t));
    for (size_t k = 0; k < 3; ++k) {
      middle[k] += weight * (vertices[t.v[0]][k] + vertices[t.v[1]][k] + vertices[t.v[2]][k]) / 3;
    }
    weights += weight;
  }
  for (size_t k = 0; k < 3; ++k) {
    middle[k] /= weights;
  }
  for (const std::vector<size_t>& piece : pieces) {
    double volume = 0;
    for (size_t i : piece) {
      const Triangle& t = triangles[i];
      const Vec c = Cross(Sub(vertices[t.v[1]], vertices[t.v[0]]), Sub(vertices[t.v[2]], vertices[t.v[0]]));
      volume += Dot(Sub(vertices[t.v[0]], middle), c);
    }
    if (volume > 0) {
      for (size_t i : piece) {
        triangles[i].Flip();
      }
    }
  }

  for (const auto& [root, group] : groups) {
    Face face;
    Vec normal{};
    double total = 0;
    std::set<Edge> directed;
    for (size_t i : group) {
      const Triangle& t = triangles[i];
      const Vec c = Cross(Sub(vertices[t.v[1]], vertices[t.v[0]]), Sub(vertices[t.v[2]], vertices[t.v[0]]));
      const double weight = std::sqrt(Dot(c, c)) / 2;
      for (size_t k = 0; k < 3; ++k) {
        normal[k] += c[k];
        face.centre[k] += weight * (vertices[t.v[0]][k] + vertices[t.v[1]][k] + vertices[t.v[2]][k]) / 3;
        face.triangles.push_back(uint8_t(t.v[k]));
        directed.insert({t.v[k], t.v[(k + 1) % 3]});
      }
      total += weight;
    }
    for (size_t k = 0; k < 3; ++k) {
      face.centre[k] /= total;
    }
    face.normal = Normalize(normal) ? normal : triangles[group[0]].normal;
    // The outline: the edges no triangle of the face runs back along, end to end.
    std::multimap<uint32_t, uint32_t> next;
    for (const Edge& edge : directed) {
      if (directed.count({edge.second, edge.first}) == 0) {
        next.insert(edge);
      }
    }
    while (!next.empty()) {
      const uint32_t start = next.begin()->first;
      std::vector<uint8_t> line = {uint8_t(start)};
      uint32_t at = start;
      for (auto found = next.find(at); found != next.end(); found = next.find(at)) {
        at = found->second;
        next.erase(found);
        line.push_back(uint8_t(at));
        if (at == start) {
          break;
        }
      }
      face.outlines.push_back(std::move(line));
    }
    faces.push_back(std::move(face));
  }
  return true;
}

// The MAPA: the disc's header and objects (doors, stations), then the corners,
// a record per face, and each face's primitives and outlines.
std::vector<uint8_t> WriteMapa(const RetailArea& retail, const std::vector<Vec>& vertices,
                               const std::vector<Face>& faces) {
  const Box box = BoxOf(vertices);
  std::vector<uint8_t> out(retail.data.begin(), retail.data.begin() + 16);  // magic, version, type, visibility
  for (const Vec& corner : {box.min, box.max}) {
    for (double c : corner) {
      PutBeFloat(out, c);
    }
  }
  AppendBE32(out, uint32_t(retail.objects));
  AppendBE32(out, uint32_t(vertices.size()));
  AppendBE32(out, uint32_t(faces.size()));
  out.insert(out.end(), retail.data.begin() + kMapaHeader,
             retail.data.begin() + kMapaHeader + retail.objects * kMapaObject);
  for (const Vec& v : vertices) {
    for (double c : v) {
      PutBeFloat(out, c);
    }
  }
  const auto pad = [](std::vector<uint8_t>& data) { data.resize((data.size() + 3) & ~size_t(3)); };
  // Offsets count from the end of the header.
  const size_t base = out.size() - kMapaHeader + faces.size() * kMapaSurface;
  std::vector<uint8_t> data;
  for (const Face& face : faces) {
    for (double c : face.normal) {
      PutBeFloat(out, c);
    }
    for (double c : face.centre) {
      PutBeFloat(out, c);
    }
    AppendBE32(out, uint32_t(base + data.size()));
    AppendBE32(data, 1);
    AppendBE32(data, kGxTriangles);
    AppendBE32(data, uint32_t(face.triangles.size()));
    data.insert(data.end(), face.triangles.begin(), face.triangles.end());
    pad(data);
    AppendBE32(out, uint32_t(base + data.size()));
    AppendBE32(data, uint32_t(face.outlines.size()));
    for (const std::vector<uint8_t>& line : face.outlines) {
      AppendBE32(data, uint32_t(line.size()));
      data.insert(data.end(), line.begin(), line.end());
      pad(data);
    }
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

// How far the corner of `from` furthest from every corner of `to` is from one.
double Furthest(const std::vector<Vec>& from, const std::vector<Vec>& to) {
  double furthest = 0;
  for (const Vec& p : from) {
    double nearest = 1e30;
    for (const Vec& q : to) {
      nearest = std::min(nearest, std::fabs(p[0] - q[0]) + std::fabs(p[1] - q[1]) + std::fabs(p[2] - q[2]));
    }
    furthest = std::max(furthest, nearest);
  }
  return furthest;
}

}  // namespace

const std::vector<MapIcon>& MapIcons() {
  // Remastered numbers its arrows by colour: 1 green, 2 magenta, 3 orange.
  static const std::vector<MapIcon> icons = {
      {"TXTR_IconS", 0x21DB7701},  // TXTR_SaveStationIcon
      {"TXTR_IconM", 0xB13B48FD},  // TXTR_MissileStationIcon
      {"TXTR_IconE", 0x0C0023F9},  // TXTR_ElevatorIcon
      {"TXTR_MapArrowDown1", PortMapIcons::ArrowId(kDownGreen)},
      {"TXTR_MapArrowUp1", PortMapIcons::ArrowId(kUpGreen)},
      {"TXTR_MapArrowDown2", PortMapIcons::ArrowId(kDownRed)},
      {"TXTR_MapArrowUp2", PortMapIcons::ArrowId(kUpRed)},
      {"TXTR_MapArrowDown3", PortMapIcons::ArrowId(kDownYellow)},
      {"TXTR_MapArrowUp3", PortMapIcons::ArrowId(kUpYellow)},
  };
  return icons;
}

std::vector<uint8_t> EncodeMapIcon(const Image& image) {
  if (image.width <= 0 || image.height <= 0 ||
      image.rgba.size() != size_t(image.width) * size_t(image.height) * 4) {
    return {};
  }
  // Resize() takes each channel on its own, and these have a soft edge over
  // transparent texels of any colour: resize premultiplied.
  Image pre = image;
  for (size_t i = 0; i < pre.rgba.size(); i += 4) {
    for (size_t c = 0; c < 3; ++c) {
      pre.rgba[i + c] = uint8_t((pre.rgba[i + c] * pre.rgba[i + 3] + 127) / 255);
    }
  }
  Image icon = Resize(pre, kIconSize, kIconSize);
  for (size_t i = 0; i < icon.rgba.size(); i += 4) {
    const int alpha = icon.rgba[i + 3];
    for (size_t c = 0; c < 3 && alpha != 0; ++c) {
      const int value = (icon.rgba[i + c] * 255 + alpha / 2) / alpha;
      icon.rgba[i + c] = uint8_t(value > 255 ? 255 : value);
    }
  }
  return EncodeTxtrRgba8(icon, 8);
}

const std::vector<MapWorld>& MapWorlds() {
  static const std::vector<MapWorld> worlds = {
      {"CMAP_IntroLevel", 0x158EFE17},     {"CMAP_RuinsLevel", 0x83F6FF6F}, {"CMAP_IceLevel", 0xA8BE6291},
      {"CMAP_OverworldLevel", 0x39F2DE28}, {"CMAP_MinesLevel", 0xB1AC4D65}, {"CMAP_LavaLevel", 0x3EF8237C},
      {"CMAP_CraterLevel", 0xC13B09D1},
  };
  return worlds;
}

bool WriteWorldMapAreas(uint32_t mlvl, const uint8_t* cmap, size_t size, const MapIO& io, int& written,
                        std::string& error) {
  const auto log = [&](const std::string& line) {
    if (io.log) {
      io.log(line);
    }
  };
  std::vector<uint8_t> raw;
  std::vector<Mat> transforms;
  uint32_t mapw = 0;
  if (!io.retail || !io.retail(kMLVL, mlvl, raw) || !ReadMlvl(raw, transforms, mapw)) {
    error = "the retail MLVL is unreadable";
    return false;
  }
  // A MAPW lists the world's MAPA in the MLVL's area order.
  if (!io.retail(kMAPW, mapw, raw) || raw.size() < 12 || ReadBE32(&raw[8]) != transforms.size() ||
      raw.size() < 12 + transforms.size() * 4) {
    error = "the retail MAPW is unreadable";
    return false;
  }
  std::vector<RetailArea> retail;
  for (size_t i = 0; i < transforms.size(); ++i) {
    RetailArea area;
    area.mapa = ReadBE32(&raw[12 + i * 4]);
    area.xf = transforms[i];
    if (!io.retail(kMAPA, area.mapa, area.data) || !ReadMapa(area) || area.vertices.empty()) {
      continue;
    }
    std::vector<Vec> world;
    for (const Vec& v : area.vertices) {
      world.push_back(ToWorld(area.xf, v));
    }
    area.world = BoxOf(world);
    retail.push_back(std::move(area));
  }
  std::vector<RemasteredArea> areas;
  if (!ReadCmap(cmap, size, areas)) {
    error = "the map is unreadable";
    return false;
  }

  // Remastered moved each world as a whole. How far: the rooms it left alone
  // are the ones whose size exactly one of its areas has.
  std::vector<double> shiftX;
  std::vector<double> shiftY;
  for (const RetailArea& r : retail) {
    const RemasteredArea* same = nullptr;
    int count = 0;
    for (const RemasteredArea& c : areas) {
      const Vec a = r.world.Extent();
      const Vec b = c.box.Extent();
      if (std::fabs(a[0] - b[0]) < 0.02 && std::fabs(a[1] - b[1]) < 0.02 && std::fabs(a[2] - b[2]) < 0.02) {
        same = &c;
        ++count;
      }
    }
    if (count == 1) {
      shiftX.push_back(r.world.min[0] - same->box.min[0]);
      shiftY.push_back(r.world.min[1] - same->box.min[1]);
    }
  }
  if (shiftX.empty()) {
    error = "none of the map's rooms is one of the disc's";
    return false;
  }
  const double shift[2] = {Median(shiftX), Median(shiftY)};

  // Pair: nearest first, by how far apart the middles are on the ground plus
  // how much the sizes differ. Height is left out, see kLevelGap.
  struct Candidate {
    double cost;
    size_t area;
    size_t retail;
    bool operator<(const Candidate& o) const {
      return cost != o.cost ? cost < o.cost : area != o.area ? area < o.area : retail < o.retail;
    }
  };
  std::vector<Candidate> candidates;
  for (size_t j = 0; j < areas.size(); ++j) {
    for (size_t i = 0; i < retail.size(); ++i) {
      const Box& c = areas[j].box;
      const Box& r = retail[i].world;
      double cost = 0;
      for (size_t k = 0; k < 2; ++k) {
        cost += std::fabs((c.min[k] + c.max[k]) / 2 + shift[k] - (r.min[k] + r.max[k]) / 2);
      }
      for (size_t k = 0; k < 3; ++k) {
        cost += std::fabs(c.Extent()[k] - r.Extent()[k]);
      }
      candidates.push_back({cost, j, i});
    }
  }
  std::sort(candidates.begin(), candidates.end());
  std::vector<bool> areaTaken(areas.size());
  std::vector<bool> retailTaken(retail.size());
  for (const Candidate& pair : candidates) {
    if (areaTaken[pair.area] || retailTaken[pair.retail]) {
      continue;
    }
    areaTaken[pair.area] = true;
    retailTaken[pair.retail] = true;
    const RemasteredArea& area = areas[pair.area];
    const RetailArea& disc = retail[pair.retail];
    if (pair.cost > kMaxPairCost) {
      log(area.name + ": none of the disc's rooms");
      continue;
    }
    const double apart = ((area.box.min[2] + area.box.max[2]) - (disc.world.min[2] + disc.world.max[2])) / 2;
    const double shiftZ = -kLevelGap * std::round(apart / kLevelGap);
    std::vector<Vec> local;
    for (const Vec& v : area.vertices) {
      local.push_back(ToArea(disc.xf, {v[0] + shift[0], v[1] + shift[1], v[2] + shiftZ}));
    }
    if (std::max(Furthest(local, disc.vertices), Furthest(disc.vertices, local)) < kSameRoom) {
      continue;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%08X.MAPA", disc.mapa);
    std::vector<Vec> vertices;
    std::vector<Face> faces;
    if (!BuildFaces(local, area.triangles, vertices, faces)) {
      log(area.name + ": too many corners for a map area");
      continue;
    }
    if (!io.write || !io.write(name, WriteMapa(disc, vertices, faces))) {
      log(std::string(name) + ": cannot write");
      continue;
    }
    ++written;
  }
  return true;
}

}  // namespace PortRemastered
