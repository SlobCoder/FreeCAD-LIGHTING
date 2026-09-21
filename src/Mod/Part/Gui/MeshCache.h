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

#ifndef PARTGUI_MESHCACHE_H
#define PARTGUI_MESHCACHE_H

#include <QByteArray>
#include <QString>

#include <TopoDS_Shape.hxx>

namespace PartGui
{

/**
 * @brief Machine-local cache for shape triangulations (sidecar files).
 *
 * Bypasses the expensive BRepMesh step on document load when a mesh computed
 * earlier (with identical shape content and meshing parameters) is already
 * on disk. The FCStd document itself is never modified; blobs live in a
 * cache directory (XDG cache by default) and can be deleted at any time.
 *
 * Preferences (User parameter:BaseApp/Preferences/Mod/Part):
 * - MeshCacheEnabled (bool, default true): master switch (local fork
 *   default; an upstream-ready patch would ship with false).
 * - MeshCacheDirectory (string, default ""): cache location; empty resolves
 *   to the user cache location + "/MeshCache".
 * - MeshCacheMaxSize (float MiB, default 1024): total size cap; oldest
 *   entries are evicted when exceeded.
 * - MeshCacheCompressionLevel (int, default 3): zstd compression level for
 *   stored blobs; 0 stores uncompressed. Reading always supports both
 *   formats. Requires a build with zstd found (FC_MESHCACHE_HAVE_ZSTD),
 *   otherwise blobs are stored uncompressed.
 *
 * Compressed blobs are a whole-file zstd frame around the regular blob
 * layout, detected via the zstd frame magic; the two formats coexist in
 * one cache directory.
 *
 * Cache keys are SHA-256 over the meshing parameters, the OCCT version and
 * a triangulation-free BRep serialization of the shape, so any topology or
 * geometry change, parameter change or OCCT upgrade invalidates the entry.
 */
class MeshCache
{
public:
    /// Meshing parameters that influence the generated triangulation.
    struct Params
    {
        double deflection;
        double angle;
        double deflectionInterior;  ///< -1.0 when not set
        double angleInterior;       ///< -1.0 when not set
        bool relative;
    };

    MeshCache();

    bool isEnabled() const
    {
        return myEnabled;
    }

    /// Attaches cached triangulations to all faces of the shape.
    /// Returns true when the shape is fully meshed afterwards
    /// (validated with BRepTools::Triangulation against theDeflection).
    bool restoreMesh(const TopoDS_Shape& shape, const Params& params);

    /// Writes the current triangulations of the shape into the cache.
    /// Uses the key computed by the last restoreMesh() call on this instance
    /// when available, otherwise computes a fresh one.
    bool storeMesh(const TopoDS_Shape& shape, const Params& params);

private:
    QByteArray computeKey(const TopoDS_Shape& shape, const Params& params) const;
    QString blobPath(const QByteArray& key) const;
    bool writeBlob(const TopoDS_Shape& shape, const Params& params) const;
    void evictIfNeeded() const;

    bool myEnabled;
    QString myDirectory;
    qint64 myMaxBytes;
    int myCompressionLevel = 0;
    QByteArray myKey;
};

}  // namespace PartGui

#endif  // PARTGUI_MESHCACHE_H
