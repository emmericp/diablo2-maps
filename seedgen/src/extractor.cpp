#include "extractor.h"
#include "d2loader.h"
#include "objects_db.h"

namespace MapData {

LevelMap ExtractLevel(Act* pAct, ActId actNo, uint32_t levelNo) {
    LevelMap m;
    m.levelNo = levelNo;
    m.actNo   = actNo;

    Level* pLevel = D2_GetLevel(pAct->pMisc, levelNo);
    if (!pLevel) return m;
    if (!pLevel->pRoom2First) D2_InitLevel(pLevel);
    if (!pLevel->pRoom2First) return m;

    m.originX = static_cast<int32_t>(pLevel->dwPosX * 5);
    m.originY = static_cast<int32_t>(pLevel->dwPosY * 5);
    m.sizeX   = static_cast<int32_t>(pLevel->dwSizeX * 5);
    m.sizeY   = static_cast<int32_t>(pLevel->dwSizeY * 5);
    if (m.sizeX <= 0 || m.sizeY <= 0) return m;

    m.coll.assign(static_cast<size_t>(m.sizeX) * m.sizeY, kNoData);
    // Initialize tight bbox to inverted bounds so any painted tile expands it.
    m.tightMinX = m.sizeX;
    m.tightMinY = m.sizeY;
    m.tightMaxX = 0;
    m.tightMaxY = 0;

    for (Room2* r = pLevel->pRoom2First; r; r = r->pRoom2Next) {
        // Room2 geometry — convert game units (×5) to tile coords, level-local.
        // pType2Info / pdwSubNumber are populated by D2 before InitLevel, so
        // we can read them without first calling AddRoomData.
        {
            Room rm{};
            rm.x     = static_cast<int32_t>(r->dwPosX  * 5) - m.originX;
            rm.y     = static_cast<int32_t>(r->dwPosY  * 5) - m.originY;
            rm.sizeX = static_cast<int32_t>(r->dwSizeX * 5);
            rm.sizeY = static_cast<int32_t>(r->dwSizeY * 5);
            if (r->pType2Info) {
                rm.roomNumber = r->pType2Info->dwRoomNumber;
                if (r->pType2Info->pdwSubNumber) rm.subNumber = *r->pType2Info->pdwSubNumber;
            }
            m.rooms.push_back(rm);
        }

        bool added = false;
        if (!r->pRoom1) {
            D2_AddRoomData(pAct, static_cast<int>(levelNo),
                           static_cast<int>(r->dwPosX), static_cast<int>(r->dwPosY));
            added = true;
        }

        if (r->pRoom1 && r->pRoom1->Coll) {
            CollMap* coll = r->pRoom1->Coll;
            const int sx = static_cast<int>(coll->dwPosGameX) - m.originX;
            const int sy = static_cast<int>(coll->dwPosGameY) - m.originY;
            const int sw = static_cast<int>(coll->dwSizeGameX);
            const int sh = static_cast<int>(coll->dwSizeGameY);
            const uint16_t* p = coll->pMapStart;
            for (int dy = 0; dy < sh; ++dy) {
                for (int dx = 0; dx < sw; ++dx, ++p) {
                    const int lx = sx + dx;
                    const int ly = sy + dy;
                    if (lx < 0 || lx >= m.sizeX || ly < 0 || ly >= m.sizeY) continue;
                    m.coll[static_cast<size_t>(ly) * m.sizeX + lx] = *p;
                    if (lx     < m.tightMinX) m.tightMinX = lx;
                    if (ly     < m.tightMinY) m.tightMinY = ly;
                    if (lx + 1 > m.tightMaxX) m.tightMaxX = lx + 1;
                    if (ly + 1 > m.tightMaxY) m.tightMaxY = ly + 1;
                }
            }
        }

        // Presets: NPCs (1), Objects (2), Exits (5). Coords are room-local in
        // tiles; convert to level-local by adding Room2 origin in tiles.
        for (PresetUnit* u = r->pPreset; u; u = u->pPresetNext) {
            const int globX = static_cast<int>(r->dwPosX * 5 + u->dwPosX);
            const int globY = static_cast<int>(r->dwPosY * 5 + u->dwPosY);
            Preset p{};
            p.x          = static_cast<int16_t>(globX - m.originX);
            p.y          = static_cast<int16_t>(globY - m.originY);
            p.type       = static_cast<uint16_t>(u->dwType);
            p.txtFileNo  = u->dwTxtFileNo;
            p.destLevelNo = 0;

            if (u->dwType == 5) {
                p.kind = ObjectKind::Stairs;
                // Find matching RoomTile to resolve destination level.
                for (RoomTile* t = r->pRoomTiles; t; t = t->pNext) {
                    if (t->nNum && *t->nNum == u->dwTxtFileNo) {
                        if (t->pRoom2 && t->pRoom2->pLevel) {
                            p.destLevelNo = t->pRoom2->pLevel->dwLevelNo;
                        }
                        break;
                    }
                }
            } else if (u->dwType == 2) {
                p.kind = ObjectKindFor(u->dwTxtFileNo);
            }
            m.presets.push_back(p);
        }

        // Adjacent levels: rooms whose pRoom2Near points to a Room2 in a
        // different level. The bridging room sits on the border between the
        // two levels — its center is a reasonable connection point.
        if (r->pRoom2Near && r->dwRoomsNear > 0) {
            const int cx = static_cast<int>(r->dwPosX * 5 + r->dwSizeX * 5 / 2) - m.originX;
            const int cy = static_cast<int>(r->dwPosY * 5 + r->dwSizeY * 5 / 2) - m.originY;
            for (uint32_t i = 0; i < r->dwRoomsNear; ++i) {
                Room2* nr = r->pRoom2Near[i];
                if (!nr || !nr->pLevel) continue;
                if (nr->pLevel->dwLevelNo == levelNo) continue;
                AdjacentArea a{};
                a.levelNo = nr->pLevel->dwLevelNo;
                a.bridgeX = static_cast<int16_t>(cx);
                a.bridgeY = static_cast<int16_t>(cy);
                m.adjacents.push_back(a);
            }
        }

        if (added) {
            D2_RemoveRoomData(pAct, static_cast<int>(levelNo),
                              static_cast<int>(r->dwPosX), static_cast<int>(r->dwPosY));
        }
    }

    if (m.tightMinX >= m.tightMaxX || m.tightMinY >= m.tightMaxY) {
        m.tightMinX = m.tightMinY = m.tightMaxX = m.tightMaxY = 0;
    }
    return m;
}

} // namespace MapData
