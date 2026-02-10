/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl_mvd.c -- MVD demo playback

#include "client.h"

mvdPlayback_t mvdPlay;

cvar_t *cl_mvdViewpoint;
cvar_t *cl_mvdTime;
cvar_t *cl_mvdDuration;

static void CL_MVD_View_f( void );
static void CL_MVD_ViewNext_f( void );
static void CL_MVD_ViewPrev_f( void );
static void CL_MVD_Seek_f( void );


/*
===============
CL_MVD_Init
===============
*/
void CL_MVD_Init( void ) {
	cl_mvdViewpoint = Cvar_Get( "cl_mvdViewpoint", "0", CVAR_ROM );
	cl_mvdTime = Cvar_Get( "cl_mvdTime", "0", CVAR_ROM );
	cl_mvdDuration = Cvar_Get( "cl_mvdDuration", "0", CVAR_ROM );
}


/*
===============
CL_MVD_ReadString

Read a null-terminated string from file, byte at a time.
===============
*/
static int CL_MVD_ReadString( char *buf, int bufSize ) {
	int i;
	for ( i = 0; i < bufSize - 1; i++ ) {
		if ( FS_Read( &buf[i], 1, mvdPlay.file ) != 1 ) {
			buf[i] = '\0';
			return -1;
		}
		if ( buf[i] == '\0' ) {
			return i;
		}
	}
	buf[bufSize - 1] = '\0';
	return i;
}


/*
===============
CL_MVD_FindFirstActivePlayer

Return the first active player clientNum from the player bitmask.
Returns -1 if none found.
===============
*/
static int CL_MVD_FindFirstActivePlayer( void ) {
	int i;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( mvdPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) {
			return i;
		}
	}
	return -1;
}


/*
===============
CL_MVD_UpdateConfigstring

Apply a configstring change to cl.gameState, rebuilding the string table.
===============
*/
static void CL_MVD_UpdateConfigstring( int index, const char *data, int dataLen ) {
	gameState_t oldGs;
	int i, len;
	const char *dup;
	char modifiedInfo[BIG_INFO_STRING];

	// Ensure \mvd\1 is always present in CS_SERVERINFO
	if ( index == CS_SERVERINFO && dataLen > 0 ) {
		Q_strncpyz( modifiedInfo, data, sizeof( modifiedInfo ) );
		Info_SetValueForKey( modifiedInfo, "mvd", "1" );
		data = modifiedInfo;
		dataLen = (int)strlen( modifiedInfo );
	}

	oldGs = cl.gameState;
	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );
	cl.gameState.dataCount = 1;

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( i == index ) {
			// use new data (may be empty string if dataLen == 0)
			if ( dataLen <= 0 ) {
				continue;
			}
			if ( dataLen + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "CL_MVD_UpdateConfigstring: MAX_GAMESTATE_CHARS exceeded" );
			}
			cl.gameState.stringOffsets[i] = cl.gameState.dataCount;
			Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, data, dataLen );
			cl.gameState.stringData[cl.gameState.dataCount + dataLen] = '\0';
			cl.gameState.dataCount += dataLen + 1;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[i];
			if ( !dup[0] ) {
				continue;
			}
			len = (int)strlen( dup );
			if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "CL_MVD_UpdateConfigstring: MAX_GAMESTATE_CHARS exceeded" );
			}
			cl.gameState.stringOffsets[i] = cl.gameState.dataCount;
			Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1 );
			cl.gameState.dataCount += len + 1;
		}
	}
}


/*
===============
CL_MVD_Open
===============
*/
qboolean CL_MVD_Open( const char *filename ) {
	char magic[4];
	int protocol;
	unsigned int indexOffsetLo, indexOffsetHi;
	char mapname[MAX_QPATH];
	char timestamp[64];
	char csData[BIG_INFO_STRING];
	unsigned short csIdx, csLen;
	unsigned int keyCount;
	int i;
	long indexOffset;
	long firstFrameOffset;

	Com_Memset( &mvdPlay, 0, sizeof( mvdPlay ) );

	if ( FS_FOpenFileRead( filename, &mvdPlay.file, qtrue ) == -1 ) {
		return qfalse;
	}

	// Read and validate magic
	if ( FS_Read( magic, 4, mvdPlay.file ) != 4 ||
		 magic[0] != 'M' || magic[1] != 'V' || magic[2] != 'D' || magic[3] != '1' ) {
		Com_Printf( S_COLOR_YELLOW "MVD: Invalid magic\n" );
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	// Protocol version
	if ( FS_Read( &protocol, 4, mvdPlay.file ) != 4 || protocol != 1 ) {
		Com_Printf( S_COLOR_YELLOW "MVD: Unsupported protocol %i\n", protocol );
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	// sv_fps
	if ( FS_Read( &mvdPlay.svFps, 4, mvdPlay.file ) != 4 ) {
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	// maxclients
	if ( FS_Read( &mvdPlay.maxclients, 4, mvdPlay.file ) != 4 ) {
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	// Index table offset (lo + hi)
	if ( FS_Read( &indexOffsetLo, 4, mvdPlay.file ) != 4 ||
		 FS_Read( &indexOffsetHi, 4, mvdPlay.file ) != 4 ) {
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	// Map name
	if ( CL_MVD_ReadString( mapname, sizeof( mapname ) ) < 0 ) {
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	// Timestamp
	if ( CL_MVD_ReadString( timestamp, sizeof( timestamp ) ) < 0 ) {
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	Com_Printf( "MVD: %s recorded %s, %i fps, %i maxclients\n",
		mapname, timestamp, mvdPlay.svFps, mvdPlay.maxclients );

	// Populate cl.gameState with configstrings
	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );
	cl.gameState.dataCount = 1;

	while ( 1 ) {
		if ( FS_Read( &csIdx, 2, mvdPlay.file ) != 2 ) {
			FS_FCloseFile( mvdPlay.file );
			return qfalse;
		}

		if ( csIdx == 0xFFFF ) {
			break;  // terminator
		}

		if ( FS_Read( &csLen, 2, mvdPlay.file ) != 2 ) {
			FS_FCloseFile( mvdPlay.file );
			return qfalse;
		}

		if ( csLen >= sizeof( csData ) ) {
			Com_Printf( S_COLOR_YELLOW "MVD: Configstring %i too long (%i)\n", csIdx, csLen );
			FS_FCloseFile( mvdPlay.file );
			return qfalse;
		}

		if ( csLen > 0 ) {
			if ( FS_Read( csData, csLen, mvdPlay.file ) != csLen ) {
				FS_FCloseFile( mvdPlay.file );
				return qfalse;
			}
		}
		csData[csLen] = '\0';

		if ( (unsigned)csIdx >= MAX_CONFIGSTRINGS ) {
			continue;
		}

		// Store in gameState
		if ( csLen + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Printf( S_COLOR_YELLOW "MVD: MAX_GAMESTATE_CHARS exceeded loading configstrings\n" );
			FS_FCloseFile( mvdPlay.file );
			return qfalse;
		}

		cl.gameState.stringOffsets[csIdx] = cl.gameState.dataCount;
		Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, csData, csLen + 1 );
		cl.gameState.dataCount += csLen + 1;
	}

	// Inject \mvd\1 into CS_SERVERINFO (CL_MVD_UpdateConfigstring auto-injects for CS_SERVERINFO)
	{
		const char *si = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
		CL_MVD_UpdateConfigstring( CS_SERVERINFO, si, (int)strlen( si ) );
	}

	// Save initial gameState and first frame offset for seeking
	mvdPlay.initialGameState = cl.gameState;
	firstFrameOffset = FS_FTell( mvdPlay.file );
	mvdPlay.firstFrameOffset = firstFrameOffset;

	// Read index table
	indexOffset = (long)indexOffsetLo;  // 32-bit files, hi is typically 0
	if ( FS_Seek( mvdPlay.file, indexOffset, FS_SEEK_SET ) != 0 ) {
		Com_Printf( S_COLOR_YELLOW "MVD: Failed to seek to index table\n" );
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	if ( FS_Read( &keyCount, 4, mvdPlay.file ) != 4 ) {
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	if ( keyCount > MAX_MVD_KEYFRAMES ) {
		keyCount = MAX_MVD_KEYFRAMES;
	}

	mvdPlay.keyframeCount = (int)keyCount;
	mvdPlay.keyframes = Z_Malloc( keyCount * sizeof( mvdKeyframe_t ) );

	for ( i = 0; i < (int)keyCount; i++ ) {
		if ( FS_Read( &mvdPlay.keyframes[i].serverTime, 4, mvdPlay.file ) != 4 ||
			 FS_Read( &mvdPlay.keyframes[i].fileOffset, 4, mvdPlay.file ) != 4 ||
			 FS_Read( &mvdPlay.keyframes[i].fileOffsetHi, 4, mvdPlay.file ) != 4 ) {
			Z_Free( mvdPlay.keyframes );
			mvdPlay.keyframes = NULL;
			FS_FCloseFile( mvdPlay.file );
			return qfalse;
		}
	}

	if ( mvdPlay.keyframeCount > 0 ) {
		mvdPlay.firstServerTime = mvdPlay.keyframes[0].serverTime;
		mvdPlay.lastServerTime = mvdPlay.keyframes[mvdPlay.keyframeCount - 1].serverTime;
	}

	// Seek back to first frame
	FS_Seek( mvdPlay.file, firstFrameOffset, FS_SEEK_SET );

	// Zero entity/player buffers
	Com_Memset( mvdPlay.entities, 0, sizeof( mvdPlay.entities ) );
	Com_Memset( mvdPlay.entityBitmask, 0, sizeof( mvdPlay.entityBitmask ) );
	Com_Memset( mvdPlay.players, 0, sizeof( mvdPlay.players ) );
	Com_Memset( mvdPlay.playerBitmask, 0, sizeof( mvdPlay.playerBitmask ) );

	// Set initial clientNum
	clc.clientNum = 0;

	// Read first frame (keyframe) and build snapshots[0]
	CL_MVD_ReadFrame();
	if ( mvdPlay.atEnd ) {
		Com_Printf( S_COLOR_YELLOW "MVD: No frames in file\n" );
		Z_Free( mvdPlay.keyframes );
		mvdPlay.keyframes = NULL;
		FS_FCloseFile( mvdPlay.file );
		return qfalse;
	}

	// Set viewpoint to first active player
	mvdPlay.viewpoint = CL_MVD_FindFirstActivePlayer();
	if ( mvdPlay.viewpoint < 0 ) {
		mvdPlay.viewpoint = 0;
	}
	clc.clientNum = mvdPlay.viewpoint;
	VectorCopy( mvdPlay.players[mvdPlay.viewpoint].origin, mvdPlay.viewOrigin );

	CL_MVD_BuildSnapshot( 0 );

	// Read second frame and build snapshots[1]
	CL_MVD_ReadFrame();
	if ( mvdPlay.atEnd ) {
		// Only one frame - duplicate
		mvdPlay.snapshots[1] = mvdPlay.snapshots[0];
		mvdPlay.snapshots[1].messageNum = mvdPlay.snapCount++;
		Com_Memcpy( mvdPlay.snapEntities[1], mvdPlay.snapEntities[0],
			sizeof( mvdPlay.snapEntities[0] ) );
	} else {
		CL_MVD_BuildSnapshot( 1 );
	}

	mvdPlay.active = qtrue;

	// Set cl.snap so CA_PRIMED -> CA_ACTIVE transition works
	cl.snap = mvdPlay.snapshots[1];
	cl.newSnapshots = qtrue;

	// Set up server message/command sequences for cgame init
	clc.serverMessageSequence = mvdPlay.snapshots[1].messageNum;
	clc.lastExecutedServerCommand = mvdPlay.cmdSequence;
	clc.serverCommandSequence = mvdPlay.cmdSequence;

	// Register commands
	Cmd_AddCommand( "mvd_view", CL_MVD_View_f );
	Cmd_AddCommand( "mvd_view_next", CL_MVD_ViewNext_f );
	Cmd_AddCommand( "mvd_view_prev", CL_MVD_ViewPrev_f );
	Cmd_AddCommand( "mvd_seek", CL_MVD_Seek_f );

	// Set duration cvar in relative ms (estimate from keyframe index; updated as frames are read)
	if ( mvdPlay.lastServerTime > mvdPlay.firstServerTime ) {
		Cvar_SetIntegerValue( "cl_mvdDuration",
			mvdPlay.lastServerTime - mvdPlay.firstServerTime );
	}
	Cvar_SetIntegerValue( "cl_mvdTime", 0 );
	Cvar_SetIntegerValue( "cl_mvdViewpoint", mvdPlay.viewpoint );

	return qtrue;
}


/*
===============
CL_MVD_Close
===============
*/
void CL_MVD_Close( void ) {
	if ( mvdPlay.file ) {
		FS_FCloseFile( mvdPlay.file );
	}

	if ( mvdPlay.keyframes ) {
		Z_Free( mvdPlay.keyframes );
	}

	Cmd_RemoveCommand( "mvd_view" );
	Cmd_RemoveCommand( "mvd_view_next" );
	Cmd_RemoveCommand( "mvd_view_prev" );
	Cmd_RemoveCommand( "mvd_seek" );

	Com_Memset( &mvdPlay, 0, sizeof( mvdPlay ) );
}


/*
===============
CL_MVD_ReadFrame

Read one frame from the current file position.
===============
*/
void CL_MVD_ReadFrame( void ) {
	unsigned int frameSize;
	msg_t msg;
	int serverTime, frameType;
	int num;
	entityState_t nullEntity;
	entityState_t tempEntity;
	playerState_t tempPlayer;
	byte oldEntityBitmask[MAX_GENTITIES/8];
	byte oldPlayerBitmask[MAX_CLIENTS/8];
	int csCount, cmdCount;
	int i;
	char csData[BIG_INFO_STRING];

	// Read frame size (4 bytes raw)
	if ( FS_Read( &frameSize, 4, mvdPlay.file ) != 4 || frameSize == 0 ) {
		mvdPlay.atEnd = qtrue;
		return;
	}

	if ( frameSize > sizeof( mvdPlay.msgBuf ) ) {
		Com_Printf( S_COLOR_YELLOW "MVD: Frame too large (%u)\n", frameSize );
		mvdPlay.atEnd = qtrue;
		return;
	}

	// Read Huffman-encoded payload
	if ( FS_Read( mvdPlay.msgBuf, frameSize, mvdPlay.file ) != (int)frameSize ) {
		mvdPlay.atEnd = qtrue;
		return;
	}

	// Set up message for reading
	MSG_Init( &msg, mvdPlay.msgBuf, sizeof( mvdPlay.msgBuf ) );
	msg.cursize = frameSize;
	MSG_BeginReading( &msg );

	// Server time
	serverTime = MSG_ReadLong( &msg );

	// Frame type: 0=delta, 1=keyframe
	frameType = MSG_ReadByte( &msg );

	if ( frameType == 1 ) {
		// Keyframe: zero all running state
		Com_Memset( mvdPlay.entities, 0, sizeof( mvdPlay.entities ) );
		Com_Memset( mvdPlay.entityBitmask, 0, sizeof( mvdPlay.entityBitmask ) );
		Com_Memset( mvdPlay.players, 0, sizeof( mvdPlay.players ) );
		Com_Memset( mvdPlay.playerBitmask, 0, sizeof( mvdPlay.playerBitmask ) );
	}

	// Flags (reserved)
	MSG_ReadByte( &msg );

	// --- Entity section ---
	Com_Memset( &nullEntity, 0, sizeof( nullEntity ) );

	// Save old bitmask for cleanup
	Com_Memcpy( oldEntityBitmask, mvdPlay.entityBitmask, sizeof( oldEntityBitmask ) );

	// Read new entity bitmask
	MSG_ReadData( &msg, mvdPlay.entityBitmask, MAX_GENTITIES / 8 );

	// Read delta-encoded entities from the bitstream
	while ( 1 ) {
		num = MSG_ReadEntitynum( &msg );
		if ( num == MAX_GENTITIES - 1 ) {
			break;  // end marker
		}

		if ( num < 0 || num >= MAX_GENTITIES - 1 ) {
			Com_Printf( S_COLOR_YELLOW "MVD: Bad entity number %i\n", num );
			mvdPlay.atEnd = qtrue;
			return;
		}

		if ( frameType == 1 ) {
			// Keyframe: delta from zeroed baseline
			MSG_ReadDeltaEntity( &msg, &nullEntity, &mvdPlay.entities[num], num );
		} else {
			// Delta frame: read into temp, then copy back
			MSG_ReadDeltaEntity( &msg, &mvdPlay.entities[num], &tempEntity, num );
			if ( tempEntity.number == MAX_GENTITIES - 1 ) {
				// Entity removed
				Com_Memset( &mvdPlay.entities[num], 0, sizeof( entityState_t ) );
			} else {
				mvdPlay.entities[num] = tempEntity;
			}
		}
	}

	// NOTE: Do NOT zero entities when they leave the bitmask.
	// The writer's prevEntities retains stale data for removed entities,
	// so deltas for reappearing entities are computed from the old state.
	// Our running state must match the writer's baseline for correct decoding.
	// The bitmask controls what goes into snapshots, so stale data is harmless.

	// --- Player section ---
	Com_Memcpy( oldPlayerBitmask, mvdPlay.playerBitmask, sizeof( oldPlayerBitmask ) );
	MSG_ReadData( &msg, mvdPlay.playerBitmask, MAX_CLIENTS / 8 );

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		int clientNum;

		if ( !( mvdPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		clientNum = MSG_ReadByte( &msg );
		if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
			Com_Printf( S_COLOR_YELLOW "MVD: Bad player clientNum %i\n", clientNum );
			mvdPlay.atEnd = qtrue;
			return;
		}

		if ( frameType == 1 ) {
			MSG_ReadDeltaPlayerstate( &msg, NULL, &mvdPlay.players[clientNum] );
		} else {
			MSG_ReadDeltaPlayerstate( &msg, &mvdPlay.players[clientNum], &tempPlayer );
			mvdPlay.players[clientNum] = tempPlayer;
		}
	}

	// NOTE: Do NOT zero players when they leave the bitmask.
	// Same reason as entities: writer's prevPlayers retains stale data.

	// Auto-switch viewpoint if current player disconnected
	if ( !( mvdPlay.playerBitmask[mvdPlay.viewpoint >> 3] & ( 1 << ( mvdPlay.viewpoint & 7 ) ) ) ) {
		int newVp = CL_MVD_FindFirstActivePlayer();
		if ( newVp >= 0 ) {
			mvdPlay.viewpoint = newVp;
			clc.clientNum = newVp;
			Cvar_SetIntegerValue( "cl_mvdViewpoint", newVp );
			}
	}

	// --- Configstring changes ---
	csCount = MSG_ReadShort( &msg );
	for ( i = 0; i < csCount; i++ ) {
		int csIndex = MSG_ReadShort( &msg );
		int csLen = MSG_ReadShort( &msg );

		if ( csLen > 0 && csLen < (int)sizeof( csData ) ) {
			MSG_ReadData( &msg, csData, csLen );
			csData[csLen] = '\0';
		} else {
			csData[0] = '\0';
		}

		if ( (unsigned)csIndex < MAX_CONFIGSTRINGS ) {
			CL_MVD_UpdateConfigstring( csIndex, csData, csLen );

			// Synthesize "cs" command for cgame so it registers new models/sounds/etc.
			// Skip during seek (mvd_seek_sync handles bulk re-registration)
			if ( !mvdPlay.seeking ) {
				char csCmd[MAX_STRING_CHARS];
				int cmdIdx;
				Com_sprintf( csCmd, sizeof( csCmd ), "cs %i \"%s\"", csIndex, csData );
				cmdIdx = mvdPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
				Q_strncpyz( mvdPlay.cmds[cmdIdx], csCmd, MAX_STRING_CHARS );
				mvdPlay.cmdSequence++;
			}
		}
	}

	// --- Server commands ---
	cmdCount = MSG_ReadShort( &msg );
	for ( i = 0; i < cmdCount; i++ ) {
		int target = MSG_ReadByte( &msg );
		int cmdLen = MSG_ReadShort( &msg );

		if ( cmdLen > 0 && cmdLen < (int)sizeof( csData ) ) {
			MSG_ReadData( &msg, csData, cmdLen );
			csData[cmdLen] = '\0';
		} else {
			csData[0] = '\0';
			cmdLen = 0;
		}

		// Queue if broadcast (255) or targeted at our viewpoint
		// Skip during seek to avoid overflowing the 64-command buffer
		if ( !mvdPlay.seeking && ( target == 255 || target == mvdPlay.viewpoint ) ) {
			int idx = mvdPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
			Q_strncpyz( mvdPlay.cmds[idx], csData, MAX_STRING_CHARS );
			mvdPlay.cmdSequence++;
		}
	}

	mvdPlay.serverTime = serverTime;

	// Track true last server time for accurate duration
	if ( serverTime > mvdPlay.lastServerTime ) {
		mvdPlay.lastServerTime = serverTime;
		Cvar_SetIntegerValue( "cl_mvdDuration",
			mvdPlay.lastServerTime - mvdPlay.firstServerTime );
	}
}


/*
===============
CL_MVD_InjectScores

Synthesize a "scores" server command from playerState data
so cgame's scoreboard always has up-to-date information.
===============
*/

// playerState_t.persistant[] indices (from game/bg_public.h)
#define MVD_PERS_SCORE					0
#define MVD_PERS_RANK					2
#define MVD_PERS_KILLED					8
#define MVD_PERS_IMPRESSIVE_COUNT		9
#define MVD_PERS_EXCELLENT_COUNT		10
#define MVD_PERS_DEFEND_COUNT			11
#define MVD_PERS_ASSIST_COUNT			12
#define MVD_PERS_GAUNTLET_FRAG_COUNT	13
#define MVD_PERS_CAPTURES				14

static void CL_MVD_InjectScores( void ) {
	char buf[MAX_STRING_CHARS];
	int len, count, i, idx;
	playerState_t *ps;
	int perfect, powerups;

	// Count active players
	count = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( mvdPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) {
			count++;
		}
	}

	// "scores <count> <redScore> <blueScore>"
	len = Com_sprintf( buf, sizeof( buf ), "scores %i 0 0", count );

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( !( mvdPlay.playerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		ps = &mvdPlay.players[i];
		perfect = ( ps->persistant[MVD_PERS_RANK] == 0 &&
					ps->persistant[MVD_PERS_KILLED] == 0 ) ? 1 : 0;
		powerups = mvdPlay.entities[i].powerups;

		len += Com_sprintf( buf + len, sizeof( buf ) - len,
			" %i %i %i %i %i %i %i %i %i %i %i %i %i %i",
			i,
			ps->persistant[MVD_PERS_SCORE],
			0,		// ping
			0,		// time
			0,		// scoreFlags
			powerups,
			0,		// accuracy
			ps->persistant[MVD_PERS_IMPRESSIVE_COUNT],
			ps->persistant[MVD_PERS_EXCELLENT_COUNT],
			ps->persistant[MVD_PERS_GAUNTLET_FRAG_COUNT],
			ps->persistant[MVD_PERS_DEFEND_COUNT],
			ps->persistant[MVD_PERS_ASSIST_COUNT],
			perfect,
			ps->persistant[MVD_PERS_CAPTURES] );
	}

	idx = mvdPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	Q_strncpyz( mvdPlay.cmds[idx], buf, MAX_STRING_CHARS );
	mvdPlay.cmdSequence++;
}


/*
===============
CL_MVD_BuildSnapshot

Build mvdPlay.snapshots[which] from current running state.
When more than MAX_ENTITIES_IN_SNAPSHOT entities are active,
keeps the nearest ones by distance from the current view origin.
===============
*/

typedef struct {
	int		entityNum;
	float	distSq;
} mvdEntDist_t;

static int MVD_EntDistCompare( const void *a, const void *b ) {
	float da = ((const mvdEntDist_t *)a)->distSq;
	float db = ((const mvdEntDist_t *)b)->distSq;
	if ( da < db ) return -1;
	if ( da > db ) return 1;
	return 0;
}

void CL_MVD_BuildSnapshot( int which ) {
	clSnapshot_t *snap;
	int count, i, total;

	// Inject synthetic scores so cgame scoreboard is always current
	CL_MVD_InjectScores();

	snap = &mvdPlay.snapshots[which];
	Com_Memset( snap, 0, sizeof( *snap ) );

	snap->valid = qtrue;
	snap->serverTime = mvdPlay.serverTime;
	snap->messageNum = mvdPlay.snapCount++;
	snap->deltaNum = snap->messageNum - 1;
	snap->snapFlags = 0;
	snap->ping = 0;
	snap->serverCommandNum = mvdPlay.cmdSequence;

	// All areas visible (0 = visible, 1 = blocked)
	snap->areabytes = MAX_MAP_AREA_BYTES;
	Com_Memset( snap->areamask, 0x00, sizeof( snap->areamask ) );

	// Player state from followed viewpoint
	snap->ps = mvdPlay.players[mvdPlay.viewpoint];
	snap->ps.clientNum = mvdPlay.viewpoint;

	// Count active entities (excluding viewpoint)
	total = 0;
	for ( i = 0; i < MAX_GENTITIES - 1; i++ ) {
		if ( !( mvdPlay.entityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) )
			continue;
		if ( i == mvdPlay.viewpoint )
			continue;
		total++;
	}

	if ( total <= MAX_ENTITIES_IN_SNAPSHOT ) {
		// All fit — simple copy, no sorting needed
		count = 0;
		for ( i = 0; i < MAX_GENTITIES - 1; i++ ) {
			if ( !( mvdPlay.entityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) )
				continue;
			if ( i == mvdPlay.viewpoint )
				continue;
			mvdPlay.snapEntities[which][count] = mvdPlay.entities[i];
			count++;
		}
	} else {
		// Too many entities — keep the nearest MAX_ENTITIES_IN_SNAPSHOT
		mvdEntDist_t candidates[MAX_GENTITIES];
		int n = 0;

		for ( i = 0; i < MAX_GENTITIES - 1; i++ ) {
			if ( !( mvdPlay.entityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) )
				continue;
			if ( i == mvdPlay.viewpoint )
				continue;
			candidates[n].entityNum = i;
			candidates[n].distSq = DistanceSquared(
				mvdPlay.viewOrigin, mvdPlay.entities[i].pos.trBase );
			n++;
		}

		qsort( candidates, n, sizeof( candidates[0] ), MVD_EntDistCompare );

		count = ( n < MAX_ENTITIES_IN_SNAPSHOT ) ? n : MAX_ENTITIES_IN_SNAPSHOT;
		for ( i = 0; i < count; i++ ) {
			mvdPlay.snapEntities[which][i] =
				mvdPlay.entities[candidates[i].entityNum];
		}
	}

	snap->numEntities = count;
	snap->parseEntitiesNum = which * MAX_ENTITIES_IN_SNAPSHOT;
}


/*
===============
CL_MVD_GetSnapshot
===============
*/
qboolean CL_MVD_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t *clSnap;
	int idx, i;

	if ( snapshotNumber == mvdPlay.snapshots[0].messageNum ) {
		idx = 0;
	} else if ( snapshotNumber == mvdPlay.snapshots[1].messageNum ) {
		idx = 1;
	} else {
		return qfalse;
	}

	clSnap = &mvdPlay.snapshots[idx];
	if ( !clSnap->valid ) {
		return qfalse;
	}

	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	Com_Memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;

	snapshot->numEntities = clSnap->numEntities;
	for ( i = 0; i < clSnap->numEntities; i++ ) {
		snapshot->entities[i] = mvdPlay.snapEntities[idx][i];
	}

	return qtrue;
}


/*
===============
CL_MVD_GetCurrentSnapshotNumber
===============
*/
void CL_MVD_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	*snapshotNumber = mvdPlay.snapshots[1].messageNum;
	*serverTime = mvdPlay.snapshots[1].serverTime;
}


/*
===============
CL_MVD_GetServerCommand
===============
*/
qboolean CL_MVD_GetServerCommand( int serverCommandNumber ) {
	const char *s;
	const char *cmd;
	static char bigConfigString[BIG_INFO_STRING];
	int index;

	if ( mvdPlay.cmdSequence - serverCommandNumber >= MAX_RELIABLE_COMMANDS ) {
		Cmd_Clear();
		return qfalse;
	}

	if ( mvdPlay.cmdSequence - serverCommandNumber < 0 ) {
		Com_Error( ERR_DROP, "CL_MVD_GetServerCommand: requested a command not received" );
		return qfalse;
	}

	index = serverCommandNumber & ( MAX_RELIABLE_COMMANDS - 1 );
	s = mvdPlay.cmds[index];
	clc.lastExecutedServerCommand = serverCommandNumber;

rescan:
	Cmd_TokenizeString( s );
	cmd = Cmd_Argv( 0 );

	if ( !strcmp( cmd, "disconnect" ) ) {
		// Ignore disconnect commands during MVD playback
		Cmd_Clear();
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs0" ) ) {
		Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv(1), Cmd_Argv(2) );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs1" ) ) {
		s = Cmd_Argv(2);
		if ( strlen( bigConfigString ) + strlen( s ) >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs2" ) ) {
		s = Cmd_Argv(2);
		if ( strlen( bigConfigString ) + strlen( s ) + 1 >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		strcat( bigConfigString, "\"" );
		s = bigConfigString;
		goto rescan;
	}

	if ( !strcmp( cmd, "cs" ) ) {
		// Apply configstring change to cl.gameState
		int csIndex = atoi( Cmd_Argv(1) );
		const char *csValue = Cmd_ArgsFrom(2);

		if ( (unsigned)csIndex < MAX_CONFIGSTRINGS ) {
			CL_MVD_UpdateConfigstring( csIndex, csValue, (int)strlen( csValue ) );
		}
		// Re-tokenize since UpdateConfigstring may have clobbered it
		Cmd_TokenizeString( s );
		return qtrue;
	}

	if ( !strcmp( cmd, "map_restart" ) ) {
		Con_ClearNotify();
		Cmd_TokenizeString( s );
		return qtrue;
	}

	return qtrue;
}


/*
===============
CL_MVD_Seek
===============
*/
void CL_MVD_Seek( int targetTime ) {
	if ( mvdPlay.keyframeCount <= 0 ) {
		return;
	}

	// Clamp
	if ( targetTime < mvdPlay.firstServerTime ) {
		targetTime = mvdPlay.firstServerTime;
	}
	if ( targetTime > mvdPlay.lastServerTime ) {
		targetTime = mvdPlay.lastServerTime;
	}

	// Restore initial gameState (configstrings are delta-encoded from header,
	// not snapshotted in keyframes, so we must replay from the beginning)
	cl.gameState = mvdPlay.initialGameState;

	// Seek to the first frame and reset all running state
	FS_Seek( mvdPlay.file, mvdPlay.firstFrameOffset, FS_SEEK_SET );
	Com_Memset( mvdPlay.entities, 0, sizeof( mvdPlay.entities ) );
	Com_Memset( mvdPlay.entityBitmask, 0, sizeof( mvdPlay.entityBitmask ) );
	Com_Memset( mvdPlay.players, 0, sizeof( mvdPlay.players ) );
	Com_Memset( mvdPlay.playerBitmask, 0, sizeof( mvdPlay.playerBitmask ) );
	mvdPlay.serverTime = 0;
	mvdPlay.atEnd = qfalse;

	// Skip command queueing during seek to avoid buffer overflow
	mvdPlay.seeking = qtrue;

	// Read ALL frames from the beginning to ensure configstrings are correct
	// (keyframes only snapshot entities/players, not configstrings)
	while ( mvdPlay.serverTime < targetTime && !mvdPlay.atEnd ) {
		CL_MVD_ReadFrame();
	}

	mvdPlay.seeking = qfalse;

	// Build both snapshots
	CL_MVD_BuildSnapshot( 0 );

	if ( !mvdPlay.atEnd ) {
		CL_MVD_ReadFrame();
		CL_MVD_BuildSnapshot( 1 );
	} else {
		mvdPlay.snapshots[1] = mvdPlay.snapshots[0];
		mvdPlay.snapshots[1].messageNum = mvdPlay.snapCount++;
		Com_Memcpy( mvdPlay.snapEntities[1], mvdPlay.snapEntities[0],
			sizeof( mvdPlay.snapEntities[0] ) );
	}

	// Inject sync command so cgame re-fetches gamestate
	{
		int idx = mvdPlay.cmdSequence & ( MAX_RELIABLE_COMMANDS - 1 );
		Com_sprintf( mvdPlay.cmds[idx], MAX_STRING_CHARS, "mvd_seek_sync %i",
			mvdPlay.viewpoint );
		mvdPlay.cmdSequence++;
	}

	// Update snapshot serverCommandNum to include the sync command
	mvdPlay.snapshots[1].serverCommandNum = mvdPlay.cmdSequence;

	// Update client state
	cl.snap = mvdPlay.snapshots[1];
	cl.newSnapshots = qtrue;
	cl.serverTimeDelta = mvdPlay.snapshots[1].serverTime - cls.realtime;
	cl.oldServerTime = mvdPlay.snapshots[0].serverTime;
	cl.oldFrameServerTime = mvdPlay.snapshots[0].serverTime;

	Cvar_SetIntegerValue( "cl_mvdTime",
		mvdPlay.serverTime - mvdPlay.firstServerTime );
}


/*
===============
CL_MVD_RebuildSnapshots

Rebuild both snapshots after a viewpoint change.
===============
*/
static void CL_MVD_RebuildSnapshots( void ) {
	int savedCount = mvdPlay.snapCount;

	// Rebuild both snapshots with new viewpoint
	mvdPlay.snapCount = savedCount - 2;
	if ( mvdPlay.snapCount < 0 ) {
		mvdPlay.snapCount = 0;
	}

	CL_MVD_BuildSnapshot( 0 );
	CL_MVD_BuildSnapshot( 1 );

	// Make messageNums consecutive
	mvdPlay.snapshots[1].messageNum = mvdPlay.snapshots[0].messageNum + 1;

	cl.snap = mvdPlay.snapshots[1];
	cl.newSnapshots = qtrue;

	clc.clientNum = mvdPlay.viewpoint;
	Cvar_SetIntegerValue( "cl_mvdViewpoint", mvdPlay.viewpoint );
}


/*
===============
CL_MVD_View_f
===============
*/
static void CL_MVD_View_f( void ) {
	int n;

	if ( !mvdPlay.active ) {
		Com_Printf( "Not playing an MVD.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "mvd_view <clientnum>\n" );
		return;
	}

	n = atoi( Cmd_Argv( 1 ) );
	if ( n < 0 || n >= MAX_CLIENTS ) {
		Com_Printf( "Invalid client number %i\n", n );
		return;
	}

	if ( !( mvdPlay.playerBitmask[n >> 3] & ( 1 << ( n & 7 ) ) ) ) {
		Com_Printf( "Client %i is not active\n", n );
		return;
	}

	mvdPlay.viewpoint = n;
	CL_MVD_RebuildSnapshots();
	Com_Printf( "MVD: Following client %i\n", n );
}


/*
===============
CL_MVD_ViewNext_f
===============
*/
static void CL_MVD_ViewNext_f( void ) {
	int i, next;

	if ( !mvdPlay.active ) {
		Com_Printf( "Not playing an MVD.\n" );
		return;
	}

	next = mvdPlay.viewpoint;
	for ( i = 1; i <= MAX_CLIENTS; i++ ) {
		int candidate = ( mvdPlay.viewpoint + i ) % MAX_CLIENTS;
		if ( mvdPlay.playerBitmask[candidate >> 3] & ( 1 << ( candidate & 7 ) ) ) {
			next = candidate;
			break;
		}
	}

	if ( next != mvdPlay.viewpoint ) {
		mvdPlay.viewpoint = next;
		CL_MVD_RebuildSnapshots();
		Com_Printf( "MVD: Following client %i\n", next );
	}
}


/*
===============
CL_MVD_ViewPrev_f
===============
*/
static void CL_MVD_ViewPrev_f( void ) {
	int i, prev;

	if ( !mvdPlay.active ) {
		Com_Printf( "Not playing an MVD.\n" );
		return;
	}

	prev = mvdPlay.viewpoint;
	for ( i = 1; i <= MAX_CLIENTS; i++ ) {
		int candidate = ( mvdPlay.viewpoint - i + MAX_CLIENTS ) % MAX_CLIENTS;
		if ( mvdPlay.playerBitmask[candidate >> 3] & ( 1 << ( candidate & 7 ) ) ) {
			prev = candidate;
			break;
		}
	}

	if ( prev != mvdPlay.viewpoint ) {
		mvdPlay.viewpoint = prev;
		CL_MVD_RebuildSnapshots();
		Com_Printf( "MVD: Following client %i\n", prev );
	}
}


/*
===============
CL_MVD_Seek_f
===============
*/
static void CL_MVD_Seek_f( void ) {
	int seconds;

	if ( !mvdPlay.active ) {
		Com_Printf( "Not playing an MVD.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "mvd_seek <seconds>\n" );
		return;
	}

	seconds = atoi( Cmd_Argv( 1 ) );
	CL_MVD_Seek( mvdPlay.firstServerTime + seconds * 1000 );
}
