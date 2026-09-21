/***************************************************************************
 *   Copyright (c) 2026 SlobCoder <slobcoder@slobcompany.example>          *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"
#ifndef _PreComp_
# include <map>
# include <sstream>
# include <utility>
# include <vector>
#endif

#include <QByteArray>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

#include <App/Application.h>
#include <Base/Console.h>
#include <Base/Parameter.h>

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepTools_ShapeSet.hxx>
#include <NCollection_Vec3.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Version.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include "MeshCache.h"

using namespace PartGui;

// The blob writer/reader uses the Poly_Triangulation API introduced in
// OCCT 7.6 (SetNode/SetUVNode/SetNormal/SetTriangle). On older versions the
// cache stays disabled.
#if OCC_VERSION_HEX >= 0x070600

namespace
{

// Binary layout (native endianness, machine-local cache):
//   header: magic[8], version u32, faceCount u32,
//           deflection, angle, deflectionInterior, angleInterior f64,
//           relative u8
//   per face: sharedFrom u32 (0 = own data, else 1-based record index of the
//             owner of the shared triangulation)
//             own data only: nodeCount i32 (0 = unmeshed face),
//             triangleCount i32, flags u8 (bit0 UV, bit1 normals),
//             deflection f64, nodes, [uv], [normals f32], triangles
//             edgePolygonCount i32, per entry: polygonCount i32 (1 or 2 for
//             seams) and that many polygons; entries follow the writer's
//             de-duplicated edge iteration order
//   polygon: nbNodes i32, deflection f64, hasParams u8, node indices,
//            [parameters f64]
constexpr char BlobMagic[8] = {'F', 'C', 'M', 'E', 'S', 'H', '0', '1'};
constexpr quint32 BlobVersion = 1;

// Returns the polygons of theEdge stored on theTriangulation. Seam edges on
// closed surfaces carry two polygons (one per seam side).
void polygonsOfEdgeOnTriangulation(
    const TopoDS_Edge& theEdge,
    const Handle(Poly_Triangulation) & theTriangulation,
    std::vector<Handle(Poly_PolygonOnTriangulation)>& thePolygons
)
{
    for (int anIndex = 1;; ++anIndex) {
        Handle(Poly_PolygonOnTriangulation) aPolygon;
        Handle(Poly_Triangulation) aRepTriangulation;
        TopLoc_Location aLocation;
        BRep_Tool::PolygonOnTriangulation(theEdge, aPolygon, aRepTriangulation, aLocation, anIndex);
        if (aPolygon.IsNull()) {
            break;
        }
        if (aRepTriangulation.get() == theTriangulation.get()) {
            thePolygons.push_back(aPolygon);
        }
    }
}

void writePolygon(QDataStream& theStream, const Handle(Poly_PolygonOnTriangulation) & thePolygon)
{
    const int aNbNodes = thePolygon->NbNodes();
    theStream << qint32(aNbNodes);
    theStream << double(thePolygon->Deflection());
    const bool hasParams = thePolygon->HasParameters();
    theStream << quint8(hasParams ? 1 : 0);
    for (int i = 1; i <= aNbNodes; ++i) {
        theStream << qint32(thePolygon->Node(i));
    }
    if (hasParams) {
        for (int i = 1; i <= aNbNodes; ++i) {
            theStream << double(thePolygon->Parameter(i));
        }
    }
}

Handle(Poly_PolygonOnTriangulation) readPolygon(QDataStream& theStream)
{
    qint32 aNbNodes = 0;
    theStream >> aNbNodes;
    if (aNbNodes <= 0) {
        return nullptr;
    }
    double aDeflection = 0.0;
    theStream >> aDeflection;
    quint8 hasParams = 0;
    theStream >> hasParams;

    TColStd_Array1OfInteger aNodes(1, aNbNodes);
    for (int i = 1; i <= aNbNodes; ++i) {
        qint32 aNode = 0;
        theStream >> aNode;
        aNodes(i) = aNode;
    }
    Handle(Poly_PolygonOnTriangulation) aPolygon;
    if (hasParams != 0) {
        TColStd_Array1OfReal aParameters(1, aNbNodes);
        for (int i = 1; i <= aNbNodes; ++i) {
            double aParameter = 0.0;
            theStream >> aParameter;
            aParameters(i) = aParameter;
        }
        aPolygon = new Poly_PolygonOnTriangulation(aNodes, aParameters);
    }
    else {
        aPolygon = new Poly_PolygonOnTriangulation(aNodes);
    }
    aPolygon->Deflection(aDeflection);
    return aPolygon;
}

}  // namespace

MeshCache::MeshCache()
{
    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Part"
    );
    myEnabled = hGrp->GetBool("MeshCacheEnabled", true);
    myDirectory = QString::fromStdString(hGrp->GetASCII("MeshCacheDirectory", ""));
    if (myDirectory.isEmpty()) {
        myDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QLatin1String("/MeshCache");
    }
    // Size cap in MiB; values <= 0 fall back to the default.
    double aMaxSizeMiB = hGrp->GetFloat("MeshCacheMaxSize", 1024.0);
    if (aMaxSizeMiB <= 0.0) {
        aMaxSizeMiB = 1024.0;
    }
    myMaxBytes = qint64(aMaxSizeMiB * 1024.0 * 1024.0);
}

QByteArray MeshCache::computeKey(const TopoDS_Shape& theShape, const Params& theParams) const
{
    // Serialize the shape without triangulation. The serialization is
    // deterministic for identical shape content, so the key survives save /
    // reload cycles while any geometry change invalidates it.
    std::ostringstream aStream;
    BRepTools_ShapeSet aShapeSet(Standard_False);
    aShapeSet.Add(theShape);
    aShapeSet.Write(aStream);
    const std::string aBrep = aStream.str();

    QCryptographicHash aHash(QCryptographicHash::Sha256);
    aHash.addData(QByteArrayView("FreeCAD-MeshCache-v1"));
    const size_t aVersionLength = std::char_traits<char>::length(OCC_VERSION_COMPLETE);
    aHash.addData(QByteArrayView(OCC_VERSION_COMPLETE, qsizetype(aVersionLength)));
    const double aValues[4]
        = {theParams.deflection, theParams.angle, theParams.deflectionInterior, theParams.angleInterior};
    aHash.addData(QByteArrayView(reinterpret_cast<const char*>(aValues), qsizetype(sizeof(aValues))));
    const quint8 aRelative = theParams.relative ? 1 : 0;
    aHash.addData(
        QByteArrayView(reinterpret_cast<const char*>(&aRelative), qsizetype(sizeof(aRelative)))
    );
    aHash.addData(QByteArrayView(aBrep.data(), qsizetype(aBrep.size())));
    return aHash.result();
}

QString MeshCache::blobPath(const QByteArray& theKey) const
{
    return myDirectory + QLatin1Char('/') + QString::fromLatin1(theKey.toHex())
        + QLatin1String(".fcmesh");
}

bool MeshCache::restoreMesh(const TopoDS_Shape& theShape, const Params& theParams)
{
    if (!myEnabled) {
        return false;
    }
    myKey = computeKey(theShape, theParams);
    const QString aPath = blobPath(myKey);

    QFile aFile(aPath);
    if (!aFile.open(QIODevice::ReadOnly)) {
        return false;  // cache miss
    }

    QDataStream aStream(&aFile);
    aStream.setByteOrder(QDataStream::LittleEndian);
    aStream.setFloatingPointPrecision(QDataStream::DoublePrecision);

    char aMagic[8] = {};
    aStream.readRawData(aMagic, 8);
    quint32 aVersion = 0;
    aStream >> aVersion;
    quint32 aFaceCount = 0;
    aStream >> aFaceCount;
    // Parameter echo written by the writer; used to validate that the blob
    // was created for the same meshing settings (guards against hash
    // collisions) and keeps the stream in sync with the header layout.
    double aBlobDeflection = 0.0, aBlobAngle = 0.0, aBlobDeflectionInterior = 0.0,
           aBlobAngleInterior = 0.0;
    quint8 aBlobRelative = 0;
    aStream >> aBlobDeflection >> aBlobAngle >> aBlobDeflectionInterior >> aBlobAngleInterior
        >> aBlobRelative;
    if (aStream.status() != QDataStream::Ok || memcmp(aMagic, BlobMagic, sizeof(BlobMagic)) != 0
        || aVersion != BlobVersion || aBlobDeflection != theParams.deflection
        || aBlobAngle != theParams.angle || aBlobDeflectionInterior != theParams.deflectionInterior
        || aBlobAngleInterior != theParams.angleInterior
        || aBlobRelative != quint8(theParams.relative ? 1 : 0)) {
        aFile.close();
        QFile::remove(aPath);
        Base::Console().log("MeshCache: removed incompatible blob %s\n", qPrintable(aPath));
        return false;
    }

    TopTools_IndexedMapOfShape aFaceMap;
    TopExp::MapShapes(theShape, TopAbs_FACE, aFaceMap);
    if (aFaceMap.Extent() != int(aFaceCount)) {
        // Topology does not match the cached entry; fall back to meshing.
        aFile.close();
        QFile::remove(aPath);
        Base::Console().log("MeshCache: face count mismatch, removed %s\n", qPrintable(aPath));
        return false;
    }

    BRep_Builder aBuilder;
    std::vector<Handle(Poly_Triangulation)> aTriangulations(aFaceCount);
    for (quint32 i = 0; i < aFaceCount; ++i) {
        const TopoDS_Face& aFace = TopoDS::Face(aFaceMap(int(i) + 1));

        quint32 aSharedFrom = 0;
        aStream >> aSharedFrom;
        if (aStream.status() != QDataStream::Ok) {
            break;
        }

        Handle(Poly_Triangulation) aTriangulation;
        if (aSharedFrom != 0) {
            // Face instance sharing the triangulation of an earlier record.
            if (aSharedFrom <= i) {
                aTriangulation = aTriangulations[aSharedFrom - 1];
            }
            if (!aTriangulation.IsNull()) {
                aBuilder.UpdateFace(aFace, aTriangulation);
            }
        }
        else {
            qint32 aNodeCount = 0;
            aStream >> aNodeCount;
            if (aStream.status() != QDataStream::Ok || aNodeCount < 0) {
                break;
            }
            if (aNodeCount > 0) {
                qint32 aTriangleCount = 0;
                aStream >> aTriangleCount;
                quint8 aFlags = 0;
                aStream >> aFlags;
                double aDeflection = 0.0;
                aStream >> aDeflection;

                const bool hasUV = (aFlags & 0x1) != 0;
                const bool hasNormals = (aFlags & 0x2) != 0;
                aTriangulation = new Poly_Triangulation(aNodeCount, aTriangleCount, hasUV, hasNormals);
                aTriangulation->Deflection(aDeflection);

                for (int n = 1; n <= aNodeCount; ++n) {
                    double x = 0.0, y = 0.0, z = 0.0;
                    aStream >> x >> y >> z;
                    aTriangulation->SetNode(n, gp_Pnt(x, y, z));
                }
                if (hasUV) {
                    for (int n = 1; n <= aNodeCount; ++n) {
                        double u = 0.0, v = 0.0;
                        aStream >> u >> v;
                        aTriangulation->SetUVNode(n, gp_Pnt2d(u, v));
                    }
                }
                if (hasNormals) {
                    for (int n = 1; n <= aNodeCount; ++n) {
                        float x = 0.F, y = 0.F, z = 0.F;
                        aStream >> x >> y >> z;
                        aTriangulation->SetNormal(n, NCollection_Vec3<float>(x, y, z));
                    }
                }
                for (int t = 1; t <= aTriangleCount; ++t) {
                    qint32 n1 = 0, n2 = 0, n3 = 0;
                    aStream >> n1 >> n2 >> n3;
                    aTriangulation->SetTriangle(t, Poly_Triangle(n1, n2, n3));
                }

                aBuilder.UpdateFace(aFace, aTriangulation);
                aTriangulations[i] = aTriangulation;
            }
        }

        if (aTriangulation.IsNull()) {
            continue;  // unmeshed face, no edge polygons either
        }

        // Restore the edge polygons of this face. The location passed to
        // UpdateEdge must be the face location; BRep_Builder::UpdateEdge()
        // derives the representation location from it the same way
        // BRep_Tool::PolygonOnTriangulation() computes its lookup key, so
        // the display code finds the polygons again.
        qint32 aEdgePolygonCount = 0;
        aStream >> aEdgePolygonCount;
        if (aStream.status() != QDataStream::Ok || aEdgePolygonCount < 0) {
            break;
        }

        TopExp_Explorer anExp(aFace, TopAbs_EDGE);
        TopTools_IndexedMapOfShape aSeenEdges;
        int aEdgeCount = 0;
        for (; anExp.More(); anExp.Next()) {
            if (aSeenEdges.Add(TopoDS::Edge(anExp.Current()))) {
                ++aEdgeCount;
            }
        }
        anExp.Init(aFace, TopAbs_EDGE);
        aSeenEdges.Clear();
        for (int p = 0; p < aEdgePolygonCount && anExp.More(); anExp.Next()) {
            const TopoDS_Edge& anEdge = TopoDS::Edge(anExp.Current());
            if (!aSeenEdges.Add(anEdge)) {
                continue;  // seam edge, already handled with both polygons
            }
            ++p;

            qint32 aPolygonCount = 0;
            aStream >> aPolygonCount;
            std::vector<Handle(Poly_PolygonOnTriangulation)> aPolygons;
            for (int e = 0; e < aPolygonCount; ++e) {
                aPolygons.push_back(readPolygon(aStream));
            }
            if (aStream.status() != QDataStream::Ok) {
                break;
            }

            const TopLoc_Location& aFaceLocation = aFace.Location();
            if (aPolygonCount >= 2 && !aPolygons[0].IsNull() && !aPolygons[1].IsNull()) {
                aBuilder.UpdateEdge(anEdge, aPolygons[0], aPolygons[1], aTriangulation, aFaceLocation);
            }
            else if (aPolygonCount == 1 && !aPolygons[0].IsNull()) {
                aBuilder.UpdateEdge(anEdge, aPolygons[0], aTriangulation, aFaceLocation);
            }
        }
    }

    if (aStream.status() != QDataStream::Ok) {
        Base::Console().log("MeshCache: truncated blob %s\n", qPrintable(aPath));
        QFile::remove(aPath);
        return false;
    }

    return BRepTools::Triangulation(theShape, theParams.deflection);
}

bool MeshCache::storeMesh(const TopoDS_Shape& theShape, const Params& theParams)
{
    if (!myEnabled) {
        return false;
    }
    if (myKey.isEmpty()) {
        myKey = computeKey(theShape, theParams);
    }
    return writeBlob(theShape, theParams);
}

bool MeshCache::writeBlob(const TopoDS_Shape& theShape, const Params& theParams) const
{
    QDir().mkpath(myDirectory);
    const QString aTargetPath = blobPath(myKey);
    const QString aTmpPath = aTargetPath + QLatin1String(".tmp")
        + QString::number(QCoreApplication::applicationPid());

    QFile aFile(aTmpPath);
    if (!aFile.open(QIODevice::WriteOnly)) {
        Base::Console().log("MeshCache: cannot write %s\n", qPrintable(aTmpPath));
        return false;
    }

    QDataStream aStream(&aFile);
    aStream.setByteOrder(QDataStream::LittleEndian);
    aStream.setFloatingPointPrecision(QDataStream::DoublePrecision);

    TopTools_IndexedMapOfShape aFaceMap;
    TopExp::MapShapes(theShape, TopAbs_FACE, aFaceMap);

    aStream.writeRawData(BlobMagic, int(sizeof(BlobMagic)));
    aStream << BlobVersion << quint32(aFaceMap.Extent());
    aStream << double(theParams.deflection) << double(theParams.angle)
            << double(theParams.deflectionInterior) << double(theParams.angleInterior);
    aStream << quint8(theParams.relative ? 1 : 0);

    // Faces sharing a triangulation (instanced shapes) store the node data
    // once; the other records reference it. Edge polygons are still stored
    // per record because their representation location differs per instance.
    std::map<const Poly_Triangulation*, quint32> aSharedOwner;
    for (int i = 1; i <= aFaceMap.Extent(); ++i) {
        const TopoDS_Face& aFace = TopoDS::Face(aFaceMap(i));
        TopLoc_Location aFaceLocation;
        Handle(Poly_Triangulation) aTriangulation = BRep_Tool::Triangulation(aFace, aFaceLocation);

        if (aTriangulation.IsNull()) {
            aStream << quint32(0)  // own record
                    << qint32(0);  // unmeshed face, no polygons
            continue;
        }

        auto anOwner = aSharedOwner.find(aTriangulation.get());
        if (anOwner != aSharedOwner.end()) {
            aStream << quint32(anOwner->second);
        }
        else {
            aSharedOwner[aTriangulation.get()] = quint32(i);
            aStream << quint32(0);

            const int aNodeCount = aTriangulation->NbNodes();
            const int aTriangleCount = aTriangulation->NbTriangles();
            const bool hasUV = aTriangulation->HasUVNodes();
            const bool hasNormals = aTriangulation->HasNormals();
            quint8 aFlags = 0;
            if (hasUV) {
                aFlags |= 0x1;
            }
            if (hasNormals) {
                aFlags |= 0x2;
            }

            aStream << qint32(aNodeCount) << qint32(aTriangleCount) << aFlags
                    << double(aTriangulation->Deflection());

            for (int n = 1; n <= aNodeCount; ++n) {
                const gp_Pnt& aNode = aTriangulation->Node(n);
                aStream << double(aNode.X()) << double(aNode.Y()) << double(aNode.Z());
            }
            if (hasUV) {
                for (int n = 1; n <= aNodeCount; ++n) {
                    const gp_Pnt2d& aUV = aTriangulation->UVNode(n);
                    aStream << double(aUV.X()) << double(aUV.Y());
                }
            }
            if (hasNormals) {
                for (int n = 1; n <= aNodeCount; ++n) {
                    NCollection_Vec3<float> aNormal;
                    aTriangulation->Normal(n, aNormal);
                    aStream << aNormal.x() << aNormal.y() << aNormal.z();
                }
            }
            for (int t = 1; t <= aTriangleCount; ++t) {
                Standard_Integer n1 = 0, n2 = 0, n3 = 0;
                aTriangulation->Triangle(t).Get(n1, n2, n3);
                aStream << qint32(n1) << qint32(n2) << qint32(n3);
            }
        }

        // Edge polygons, de-duplicated by edge within the face (seam edges
        // appear twice in the explorer but share one record with up to two
        // polygons, one per seam side).
        std::vector<TopoDS_Edge> aPolygonEdges;
        std::vector<std::vector<Handle(Poly_PolygonOnTriangulation)>> aEdgePolygons;
        TopExp_Explorer anExp(aFace, TopAbs_EDGE);
        TopTools_IndexedMapOfShape aSeenEdges;
        for (; anExp.More(); anExp.Next()) {
            const TopoDS_Edge& anEdge = TopoDS::Edge(anExp.Current());
            if (!aSeenEdges.Add(anEdge)) {
                continue;
            }
            std::vector<Handle(Poly_PolygonOnTriangulation)> aPolygons;
            polygonsOfEdgeOnTriangulation(anEdge, aTriangulation, aPolygons);
            aPolygonEdges.push_back(anEdge);
            aEdgePolygons.push_back(std::move(aPolygons));
        }

        aStream << qint32(aPolygonEdges.size());
        for (const auto& aPolygons : aEdgePolygons) {
            aStream << qint32(aPolygons.size());
            for (const auto& aPolygon : aPolygons) {
                writePolygon(aStream, aPolygon);
            }
        }
    }

    aFile.close();
    if (aFile.error() != QFile::NoError) {
        QFile::remove(aTmpPath);
        Base::Console().log("MeshCache: write error on %s\n", qPrintable(aTmpPath));
        return false;
    }

    QFile::remove(aTargetPath);  // no-op if absent; makes rename portable
    if (!QFile::rename(aTmpPath, aTargetPath)) {
        QFile::remove(aTmpPath);
        return false;
    }
    evictIfNeeded();
    return true;
}

void MeshCache::evictIfNeeded() const
{
    QDir aDir(myDirectory);
    const QFileInfoList aEntries = aDir.entryInfoList(
        QStringList() << QLatin1String("*.fcmesh"),
        QDir::Files,
        QDir::Time | QDir::Reversed
    );  // oldest first
    qint64 aTotal = 0;
    for (const QFileInfo& anInfo : aEntries) {
        aTotal += anInfo.size();
    }
    for (const QFileInfo& anInfo : aEntries) {
        if (aTotal <= myMaxBytes) {
            break;
        }
        aTotal -= anInfo.size();
        QFile::remove(anInfo.absoluteFilePath());
        Base::Console().log("MeshCache: evicted %s\n", qPrintable(anInfo.fileName()));
    }
}

#else  // OCC_VERSION_HEX >= 0x070600

MeshCache::MeshCache()
    : myEnabled(false)
    , myMaxBytes(0)
{}

bool MeshCache::restoreMesh(const TopoDS_Shape&, const Params&)
{
    return false;
}

bool MeshCache::storeMesh(const TopoDS_Shape&, const Params&)
{
    return false;
}

#endif  // OCC_VERSION_HEX >= 0x070600
