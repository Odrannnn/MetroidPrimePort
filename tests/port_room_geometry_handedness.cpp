#include "port_room_geometry_handedness.h"
#include "port_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace {
GXCullMode sCull = GX_CULL_FRONT;
unsigned sChanges = 0;
struct Matrix {
  double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double Get00() const { return m[0]; }
  double Get01() const { return m[1]; }
  double Get02() const { return m[2]; }
  double Get10() const { return m[3]; }
  double Get11() const { return m[4]; }
  double Get12() const { return m[5]; }
  double Get20() const { return m[6]; }
  double Get21() const { return m[7]; }
  double Get22() const { return m[8]; }
};
using Point = std::array<double, 2>;
using Triangle = std::array<size_t, 3>;
double Area(const std::vector<Point>& points, const Triangle& t, bool mirror) {
  const Point& a = points[t[0]];
  const Point& b = points[t[1]];
  const Point& c = points[t[2]];
  return ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) * (mirror ? -1 : 1);
}
void CheckPrimitive(uint8_t opcode, const std::vector<Point>& points, const std::vector<Triangle>& triangles) {
  using namespace PortRoomGeometryHandedness;
  std::vector<size_t> order;
  uint8_t encoded = 0;
  REQUIRE(PrimitiveOrder(opcode | 4, points.size(), true, encoded, order));
  CHECK(encoded == (GX_TRIANGLES | 4));
  REQUIRE(order.size() == triangles.size() * 3);
  for (size_t i = 0; i < triangles.size(); ++i) {
    Triangle got{order[i * 3], order[i * 3 + 1], order[i * 3 + 2]};
    CHECK(std::fabs(Area(points, got, true) - Area(points, triangles[i], false)) < 1e-9);
    auto expected = triangles[i];
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    CHECK(got == expected); // preserve the triangles and all attached attribute indices
  }
  REQUIRE(PrimitiveOrder(opcode | 4, points.size(), false, encoded, order));
  CHECK(encoded == (opcode | 4));
  REQUIRE(order.size() == points.size());
  for (size_t i = 0; i < order.size(); ++i) CHECK(order[i] == i);
}
void CullAndQueue() {
  using namespace PortRoomGeometryHandedness;
  for (const GXCullMode initial : {GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_NONE, GX_CULL_ALL}) {
    sCull = initial;
    sChanges = 0;
    std::vector<GXCullMode> queued;
    {
      const CullScope scope(true);
      queued.push_back(sCull); // color, fallback, depth and shadow pipeline take this value
      const GXCullMode expected = initial == GX_CULL_FRONT ? GX_CULL_BACK : initial == GX_CULL_BACK ? GX_CULL_FRONT : initial;
      CHECK(sCull == expected);
      {
        const CullScope nested(true);
        CHECK(sCull == initial);
      }
      CHECK(sCull == expected);
    }
    CHECK(sCull == initial);
    CHECK(queued.front() == (initial == GX_CULL_FRONT ? GX_CULL_BACK : initial == GX_CULL_BACK ? GX_CULL_FRONT : initial));
    CHECK(sChanges == (initial == GX_CULL_NONE || initial == GX_CULL_ALL ? 0u : 4u));
    {
      const CullScope normal(false);
      CHECK(sCull == initial);
    }
    CHECK(sCull == initial);
  }
}

using Bytes = std::vector<uint8_t>;
using PortRoomGeometryHandedness::AppendPrimitive;
using PortRoomGeometryHandedness::PrimitiveLayout;
uint16_t ReadIndex(std::span<const uint8_t> bytes, size_t at) {
  return uint16_t(uint16_t(bytes[at]) << 8 | bytes[at + 1]);
}
void WriteIndex(Bytes& bytes, size_t at, uint16_t index) {
  bytes[at] = uint8_t(index >> 8);
  bytes[at + 1] = uint8_t(index);
}
Bytes AttributeRecords(size_t count, size_t stride) {
  Bytes bytes(count * stride);
  for (size_t i = 0; i < count; ++i) {
    WriteIndex(bytes, i * stride, uint16_t(i));
    if (stride >= 4) WriteIndex(bytes, i * stride + 2, uint16_t(i * 17 + 1));
    for (size_t byte = 4; byte < stride; ++byte) bytes[i * stride + byte] = uint8_t(i * 31 + byte * 7 + 0x43);
  }
  return bytes;
}
struct Emitted {
  uint8_t opcode;
  size_t count;
  size_t data;
};
std::vector<Emitted> Decode(const Bytes& bytes, size_t stride) {
  std::vector<Emitted> primitives;
  for (size_t at = 0; at < bytes.size();) {
    REQUIRE(bytes.size() - at >= 3);
    const size_t count = ReadIndex(bytes, at + 1);
    REQUIRE(count != 0 && count <= 0xffff);
    REQUIRE(count <= (bytes.size() - at - 3) / stride);
    primitives.push_back({bytes[at], count, at + 3});
    at += 3 + count * stride;
  }
  return primitives;
}
void CheckRecord(const Bytes& bytes, size_t at, const Bytes& source, size_t sourceIndex,
                 const PrimitiveLayout& layout) {
  const size_t src = sourceIndex * layout.stride;
  CHECK(ReadIndex(bytes, at) == ReadIndex(source, src) + layout.positionBase);
  if (layout.normalAt != SIZE_MAX) {
    CHECK(ReadIndex(bytes, at + layout.normalAt) == ReadIndex(source, src + layout.normalAt) + layout.normalBase);
  }
  for (size_t byte = 2; byte < layout.stride; ++byte) {
    if (layout.normalAt != SIZE_MAX && (byte == layout.normalAt || byte == layout.normalAt + 1)) continue;
    CHECK(bytes[at + byte] == source[src + byte]); // UV/color records stay attached
  }
}
void MixedMergedCopies() {
  const Bytes source = AttributeRecords(4, 10);
  const PrimitiveLayout ordinary{10, 2, 100, 200};
  const PrimitiveLayout reflected{10, 2, 1000, 2000};
  Bytes bytes;
  REQUIRE(AppendPrimitive(GX_TRIANGLESTRIP | 6, 4, source, ordinary, false, bytes));
  REQUIRE(AppendPrimitive(GX_TRIANGLESTRIP | 6, 4, source, reflected, true, bytes));
  const auto emitted = Decode(bytes, 10);
  REQUIRE(emitted.size() == 2);
  CHECK(emitted[0].opcode == (GX_TRIANGLESTRIP | 6));
  CHECK(emitted[0].count == 4);
  for (size_t i = 0; i < 4; ++i) CheckRecord(bytes, emitted[0].data + i * 10, source, i, ordinary);
  CHECK(emitted[1].opcode == (GX_TRIANGLES | 6));
  CHECK(emitted[1].count == 6);
  const size_t expected[] = {0, 2, 1, 2, 3, 1};
  for (size_t i = 0; i < 6; ++i) CheckRecord(bytes, emitted[1].data + i * 10, source, expected[i], reflected);

  // Without a normal array the second field is a UV/color index, not a normal.
  const PrimitiveLayout noNormals{10, SIZE_MAX, 400, SIZE_MAX};
  const Bytes fan = AttributeRecords(5, 10);
  bytes.clear();
  REQUIRE(AppendPrimitive(GX_TRIANGLEFAN | 5, 5, fan, noNormals, true, bytes));
  const auto result = Decode(bytes, 10);
  REQUIRE(result.size() == 1 && result[0].count == 9);
  const size_t fanOrder[] = {0, 2, 1, 0, 3, 2, 0, 4, 3};
  for (size_t i = 0; i < 9; ++i) CheckRecord(bytes, result[0].data + i * 10, fan, fanOrder[i], noNormals);

  // PBIX-style bases may reach the final representable position/normal entry.
  Bytes boundary = AttributeRecords(3, 10);
  for (size_t i = 0; i < 3; ++i) WriteIndex(boundary, i * 10 + 2, uint16_t(i));
  const PrimitiveLayout edge{10, 2, 65533, 65533};
  bytes.clear();
  REQUIRE(AppendPrimitive(GX_TRIANGLES | 3, 3, boundary, edge, true, bytes));
  const auto last = Decode(bytes, 10);
  REQUIRE(last.size() == 1 && last[0].count == 3);
  CheckRecord(bytes, last[0].data, boundary, 0, edge);
  CheckRecord(bytes, last[0].data + 10, boundary, 2, edge);
  CheckRecord(bytes, last[0].data + 20, boundary, 1, edge);
}
void ExpandedPrimitiveBoundaries(uint8_t opcode, size_t count) {
  const Bytes source = AttributeRecords(count, 6);
  const PrimitiveLayout layout{6, SIZE_MAX, 0, 0};
  Bytes bytes;
  REQUIRE(AppendPrimitive(opcode | 3, count, source, layout, true, bytes));
  const auto emitted = Decode(bytes, 6);
  REQUIRE(emitted.size() > 1);
  size_t vertex = 0;
  for (const auto& primitive : emitted) {
    CHECK(primitive.opcode == (GX_TRIANGLES | 3));
    CHECK(primitive.count % 3 == 0);
    for (size_t i = 0; i < primitive.count; ++i, ++vertex) {
      const size_t triangle = vertex / 3, corner = vertex % 3;
      size_t expected;
      if (opcode == GX_TRIANGLEFAN) {
        expected = corner == 0 ? 0 : corner == 1 ? triangle + 2 : triangle + 1;
      } else if ((triangle & 1) == 0) {
        expected = triangle + (corner == 0 ? 0 : corner == 1 ? 2 : 1);
      } else {
        expected = triangle + (corner == 0 ? 1 : corner == 1 ? 2 : 0);
      }
      CheckRecord(bytes, primitive.data + i * 6, source, expected, layout);
    }
  }
  CHECK(vertex == 3 * (count - 2));
}
void InvalidMergedInput() {
  const Bytes source = AttributeRecords(4, 10);
  const Bytes prefix{0x12, 0x34, 0x56};
  Bytes bytes = prefix;
  const PrimitiveLayout valid{10, 2, 0, 0};
  const auto reject = [&](uint8_t op, size_t count, std::span<const uint8_t> records,
                          const PrimitiveLayout& layout, bool mirrored = true) {
    CHECK(!AppendPrimitive(op, count, records, layout, mirrored, bytes));
    CHECK(bytes == prefix);
  };
  reject(GX_QUADS, 4, std::span(source).first(39), valid);
  Bytes extra = source;
  extra.push_back(0);
  reject(GX_QUADS, 4, extra, valid);
  reject(GX_QUADS, 4, source, {1, SIZE_MAX, 0, 0});
  reject(GX_QUADS, 4, source, {10, 0, 0, 0});
  reject(GX_QUADS, 4, source, {10, 1, 0, 0});
  reject(GX_QUADS, 4, source, {10, 9, 0, 0});
  reject(GX_QUADS, 4, source, {SIZE_MAX, SIZE_MAX, 0, 0});
  reject(GX_QUADS, 4, source, {10, 2, SIZE_MAX, 0});
  reject(GX_QUADS, 4, source, {10, 2, 0, SIZE_MAX});
  reject(GX_QUADS, 4, source, {10, 2, 65533, 0}); // overflow at the final position
  reject(GX_QUADS, 4, source, {10, 2, 0, 65500}); // overflow after valid normal indices
  reject(GX_TRIANGLES, 4, source, valid);
  reject(0x88, 4, source, valid);
  reject(0x88, 4, source, valid, false);
  reject(GX_LINES, 3, std::span(source).first(30), valid);
  reject(GX_TRIANGLESTRIP, 2, std::span(source).first(20), valid);
  reject(GX_TRIANGLES, 0, {}, valid);
  reject(GX_TRIANGLES, SIZE_MAX, {}, valid);
  const Bytes overCount = AttributeRecords(65536, 10);
  reject(GX_TRIANGLESTRIP, 65536, overCount, valid);

  // An aliased source would be invalidated if the output allocation grew.
  const Bytes aliasedBefore = bytes;
  CHECK(!AppendPrimitive(GX_POINTS, 1, std::span(bytes).first(2), {2}, false, bytes));
  CHECK(bytes == aliasedBefore);
}
} // namespace

extern "C" void GXGetCullMode(GXCullMode* mode) { *mode = sCull; }
extern "C" void GXSetCullMode(GXCullMode mode) { sCull = mode; ++sChanges; }

int main() {
  using namespace PortRoomGeometryHandedness;
  Matrix matrix;
  CHECK(!Mirrored(matrix));
  matrix.m[0] = -1;
  CHECK(Mirrored(matrix));
  matrix.m[4] = -2;
  CHECK(!Mirrored(matrix)); // two reflected axes preserve winding
  matrix.m[8] = 0;
  CHECK(!Mirrored(matrix)); // degenerate transforms do not invent a handedness
  matrix = {{0, 2, 0, 3, 0, 0, 0, 0, 4}};
  CHECK(Mirrored(matrix)); // reflected rotated nonuniform instance
  CullAndQueue();
  CheckPrimitive(GX_TRIANGLES, {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}}, {{0, 1, 2}, {3, 4, 5}});
  CheckPrimitive(GX_QUADS, {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {{0, 1, 2}, {0, 2, 3}});
  CheckPrimitive(GX_TRIANGLESTRIP, {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}}, {{0, 1, 2}, {2, 1, 3}, {2, 3, 4}});
  CheckPrimitive(GX_TRIANGLESTRIP, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 1, 2}, {2, 1, 3}});
  CheckPrimitive(GX_TRIANGLEFAN, {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}}, {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}});
  std::vector<size_t> order;
  uint8_t encoded = 0;
  CHECK(!PrimitiveOrder(GX_TRIANGLES, 4, true, encoded, order));
  CHECK(!PrimitiveOrder(GX_QUADS, 3, true, encoded, order));
  CHECK(!PrimitiveOrder(GX_TRIANGLESTRIP, 2, true, encoded, order));
  CHECK(!PrimitiveOrder(0x88, 4, true, encoded, order));
  REQUIRE(PrimitiveOrder(GX_TRIANGLEFAN | 3, 65535, true, encoded, order));
  CHECK(order.size() == 3 * (65535 - 2));
  CHECK(*std::max_element(order.begin(), order.end()) == 65534);
  for (const uint8_t primitive : {uint8_t(GX_LINES), uint8_t(GX_LINESTRIP), uint8_t(GX_POINTS)}) {
    REQUIRE(PrimitiveOrder(primitive, 4, true, encoded, order));
    CHECK(encoded == primitive);
    CHECK(order == std::vector<size_t>({0, 1, 2, 3}));
  }
  MixedMergedCopies();
  for (const size_t count : {size_t(65534), size_t(65535)}) {
    ExpandedPrimitiveBoundaries(GX_TRIANGLESTRIP, count);
    ExpandedPrimitiveBoundaries(GX_TRIANGLEFAN, count);
  }
  InvalidMergedInput();
  std::puts("port_room_geometry_handedness: ok");
  return PORT_TEST_RESULT();
}
