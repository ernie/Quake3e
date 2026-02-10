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

#include "server.h"

mvdState_t mvd;
cvar_t *sv_mvdauto;


/*
===============
SV_MVD_Init
===============
*/
void SV_MVD_Init( void ) {
	sv_mvdauto = Cvar_Get( "sv_mvdauto", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( sv_mvdauto, "Automatically start MVD recording on map load." );
}


/*
===============
SV_MVD_FileWrite

Write data to MVD file and track file offset.
===============
*/
static void SV_MVD_FileWrite( const void *data, int len, fileHandle_t f ) {
	unsigned int newOffset;

	FS_Write( data, len, f );

	newOffset = mvd.fileOffset + (unsigned int)len;
	if ( newOffset < mvd.fileOffset ) {
		mvd.fileOffsetHi++;
	}
	mvd.fileOffset = newOffset;
}


/*
===============
SV_MVD_StartRecord
===============
*/
static void SV_MVD_StartRecord( const char *filename ) {
	char path[MAX_QPATH];
	int i;
	int val;
	unsigned int zero8[2] = { 0, 0 };
	char timestamp[64];
	time_t now;
	struct tm *tm_info;

	if ( sv.state != SS_GAME ) {
		Com_Printf( "MVD: Not recording, server not running.\n" );
		return;
	}

	if ( mvd.recording ) {
		Com_Printf( "MVD: Already recording.\n" );
		return;
	}

	Com_Memset( &mvd, 0, sizeof( mvd ) );

	Com_sprintf( path, sizeof( path ), "demos/%s.mvd", filename );
	mvd.file = FS_FOpenFileWrite( path );
	if ( !mvd.file ) {
		Com_Printf( "MVD: Could not open %s for writing.\n", path );
		return;
	}

	mvd.fileOffset = 0;
	mvd.fileOffsetHi = 0;

	// Write header: magic
	SV_MVD_FileWrite( "MVD1", 4, mvd.file );

	// Protocol version
	val = 1;
	SV_MVD_FileWrite( &val, 4, mvd.file );

	// sv_fps
	val = sv_fps->integer;
	SV_MVD_FileWrite( &val, 4, mvd.file );

	// maxclients
	val = sv.maxclients;
	SV_MVD_FileWrite( &val, 4, mvd.file );

	// Index table offset placeholder (8 bytes)
	mvd.indexOffsetPos = mvd.fileOffset;
	SV_MVD_FileWrite( zero8, 8, mvd.file );

	// Map name (null-terminated)
	SV_MVD_FileWrite( sv_mapname->string, (int)strlen( sv_mapname->string ) + 1, mvd.file );

	// Timestamp (null-terminated ISO 8601)
	now = time( NULL );
	tm_info = localtime( &now );
	strftime( timestamp, sizeof( timestamp ), "%Y-%m-%dT%H:%M:%S", tm_info );
	SV_MVD_FileWrite( timestamp, (int)strlen( timestamp ) + 1, mvd.file );

	// Write configstrings
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		int len;
		unsigned short idx, slen;

		if ( !sv.configstrings[i] || sv.configstrings[i][0] == '\0' ) {
			continue;
		}

		len = (int)strlen( sv.configstrings[i] );
		idx = (unsigned short)i;
		slen = (unsigned short)len;
		SV_MVD_FileWrite( &idx, 2, mvd.file );
		SV_MVD_FileWrite( &slen, 2, mvd.file );
		SV_MVD_FileWrite( sv.configstrings[i], len, mvd.file );
	}

	// Configstring terminator
	{
		unsigned short term = 0xFFFF;
		SV_MVD_FileWrite( &term, 2, mvd.file );
	}

	// Zero baselines
	Com_Memset( mvd.prevEntities, 0, sizeof( mvd.prevEntities ) );
	Com_Memset( mvd.prevEntityBitmask, 0, sizeof( mvd.prevEntityBitmask ) );
	Com_Memset( mvd.prevPlayers, 0, sizeof( mvd.prevPlayers ) );
	Com_Memset( mvd.prevPlayerBitmask, 0, sizeof( mvd.prevPlayerBitmask ) );

	// Clear per-frame state
	Com_Memset( mvd.csChanged, 0, sizeof( mvd.csChanged ) );
	mvd.cmdCount = 0;
	mvd.cmdBufUsed = 0;

	mvd.recording = qtrue;
	mvd.frameCount = 0;
	mvd.keyframeCount = 0;

	Com_Printf( "MVD: Recording to %s\n", path );
}


/*
===============
SV_MVD_StartRecord_f
===============
*/
void SV_MVD_StartRecord_f( void ) {
	const char *filename;
	char defaultName[MAX_QPATH];

	if ( Cmd_Argc() >= 2 ) {
		filename = Cmd_Argv( 1 );
	} else {
		time_t now = time( NULL );
		struct tm *tm_info = localtime( &now );
		strftime( defaultName, sizeof( defaultName ), "mvd_%Y%m%d_%H%M%S", tm_info );
		filename = defaultName;
	}

	SV_MVD_StartRecord( filename );
}


/*
===============
SV_MVD_WriteFrame
===============
*/
void SV_MVD_WriteFrame( void ) {
	msg_t msg;
	int i;
	qboolean keyframe;
	entityState_t nullEntity;
	byte curEntityBitmask[MAX_GENTITIES/8];
	byte curPlayerBitmask[MAX_CLIENTS/8];
	int csCount;
	unsigned int frameSize;

	if ( !mvd.recording ) {
		return;
	}

	keyframe = ( mvd.frameCount % MVD_KEYFRAME_INTERVAL ) == 0;

	// Record keyframe position
	if ( keyframe && mvd.keyframeCount < MAX_MVD_KEYFRAMES ) {
		mvd.keyframes[mvd.keyframeCount].serverTime = sv.time;
		mvd.keyframes[mvd.keyframeCount].fileOffset = mvd.fileOffset;
		mvd.keyframes[mvd.keyframeCount].fileOffsetHi = mvd.fileOffsetHi;
		mvd.keyframeCount++;
	}

	// Init message buffer
	MSG_Init( &msg, mvd.msgBuf, MAX_MVD_MSGLEN );
	MSG_Bitstream( &msg );

	// Write server time
	MSG_WriteLong( &msg, sv.time );

	// Frame type: 0=delta, 1=keyframe
	MSG_WriteByte( &msg, keyframe ? 1 : 0 );

	// Flags (reserved)
	MSG_WriteByte( &msg, 0 );

	// --- Entity encoding ---
	Com_Memset( &nullEntity, 0, sizeof( nullEntity ) );
	Com_Memset( curEntityBitmask, 0, sizeof( curEntityBitmask ) );

	// Build current entity bitmask
	for ( i = 0; i < sv.num_entities; i++ ) {
		sharedEntity_t *ent = SV_GentityNum( i );
		if ( ent->r.linked && !( ent->r.svFlags & SVF_NOCLIENT ) ) {
			curEntityBitmask[i >> 3] |= ( 1 << ( i & 7 ) );
		}
	}

	// Write entity bitmask
	MSG_WriteData( &msg, curEntityBitmask, sizeof( curEntityBitmask ) );

	// Write delta-encoded entities
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		entityState_t *es;

		if ( !( curEntityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		es = &SV_GentityNum( i )->s;

		if ( keyframe ) {
			MSG_WriteDeltaEntity( &msg, &nullEntity, es, qtrue );
		} else {
			MSG_WriteDeltaEntity( &msg, &mvd.prevEntities[i], es, qfalse );
		}
	}

	// Entity end marker
	MSG_WriteBits( &msg, MAX_GENTITIES - 1, GENTITYNUM_BITS );

	// --- Player encoding ---
	Com_Memset( curPlayerBitmask, 0, sizeof( curPlayerBitmask ) );

	for ( i = 0; i < sv.maxclients; i++ ) {
		if ( svs.clients[i].state == CS_ACTIVE ) {
			curPlayerBitmask[i >> 3] |= ( 1 << ( i & 7 ) );
		}
	}

	// Write player bitmask
	MSG_WriteData( &msg, curPlayerBitmask, sizeof( curPlayerBitmask ) );

	// Write delta-encoded players
	for ( i = 0; i < sv.maxclients; i++ ) {
		playerState_t *ps;

		if ( !( curPlayerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) ) {
			continue;
		}

		ps = SV_GameClientNum( i );
		MSG_WriteByte( &msg, i );

		if ( keyframe ) {
			MSG_WriteDeltaPlayerstate( &msg, NULL, ps );
		} else {
			MSG_WriteDeltaPlayerstate( &msg, &mvd.prevPlayers[i], ps );
		}
	}

	// --- Configstring changes ---
	csCount = 0;
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( mvd.csChanged[i] ) {
			csCount++;
		}
	}

	MSG_WriteShort( &msg, csCount );

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		int len;

		if ( !mvd.csChanged[i] ) {
			continue;
		}

		len = sv.configstrings[i] ? (int)strlen( sv.configstrings[i] ) : 0;
		MSG_WriteShort( &msg, i );
		MSG_WriteShort( &msg, len );
		if ( len > 0 ) {
			MSG_WriteData( &msg, sv.configstrings[i], len );
		}
	}

	Com_Memset( mvd.csChanged, 0, sizeof( mvd.csChanged ) );

	// --- Server commands ---
	MSG_WriteShort( &msg, mvd.cmdCount );

	for ( i = 0; i < mvd.cmdCount; i++ ) {
		MSG_WriteByte( &msg, mvd.cmds[i].target == -1 ? 255 : (byte)mvd.cmds[i].target );
		MSG_WriteShort( &msg, mvd.cmds[i].len );
		MSG_WriteData( &msg, mvd.cmdBuf + mvd.cmds[i].offset, mvd.cmds[i].len );
	}

	mvd.cmdCount = 0;
	mvd.cmdBufUsed = 0;

	// --- Flush to file ---
	if ( msg.overflowed ) {
		Com_Printf( "MVD: Frame %i overflowed message buffer, stopping recording.\n", mvd.frameCount );
		SV_MVD_StopRecord();
		return;
	}

	frameSize = (unsigned int)msg.cursize;
	SV_MVD_FileWrite( &frameSize, 4, mvd.file );
	SV_MVD_FileWrite( msg.data, msg.cursize, mvd.file );

	// Save current state as previous for next delta.
	// Zero removed entities/players so reappearing ones get a full delta.
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		if ( curEntityBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) {
			mvd.prevEntities[i] = SV_GentityNum( i )->s;
		} else {
			Com_Memset( &mvd.prevEntities[i], 0, sizeof( entityState_t ) );
		}
	}
	Com_Memcpy( mvd.prevEntityBitmask, curEntityBitmask, sizeof( curEntityBitmask ) );

	for ( i = 0; i < sv.maxclients; i++ ) {
		if ( curPlayerBitmask[i >> 3] & ( 1 << ( i & 7 ) ) ) {
			mvd.prevPlayers[i] = *SV_GameClientNum( i );
		} else {
			Com_Memset( &mvd.prevPlayers[i], 0, sizeof( playerState_t ) );
		}
	}
	Com_Memcpy( mvd.prevPlayerBitmask, curPlayerBitmask, sizeof( curPlayerBitmask ) );

	mvd.frameCount++;
}


/*
===============
SV_MVD_StopRecord
===============
*/
void SV_MVD_StopRecord( void ) {
	unsigned int indexOffset[2];
	int i;
	unsigned int count;
	float duration;

	if ( !mvd.recording ) {
		return;
	}

	// Record index table file offset
	indexOffset[0] = mvd.fileOffset;
	indexOffset[1] = mvd.fileOffsetHi;

	// Write index table
	count = (unsigned int)mvd.keyframeCount;
	SV_MVD_FileWrite( &count, 4, mvd.file );

	for ( i = 0; i < mvd.keyframeCount; i++ ) {
		SV_MVD_FileWrite( &mvd.keyframes[i].serverTime, 4, mvd.file );
		SV_MVD_FileWrite( &mvd.keyframes[i].fileOffset, 4, mvd.file );
		SV_MVD_FileWrite( &mvd.keyframes[i].fileOffsetHi, 4, mvd.file );
	}

	// Patch header with index table offset
	FS_Seek( mvd.file, (long)mvd.indexOffsetPos, FS_SEEK_SET );
	FS_Write( indexOffset, 8, mvd.file );

	FS_FCloseFile( mvd.file );

	duration = (float)mvd.frameCount / (float)sv_fps->integer;

	Com_Printf( "MVD: Recording stopped. %i frames (%.1f seconds), %u bytes.\n",
		mvd.frameCount, duration, mvd.fileOffset );

	mvd.recording = qfalse;
	mvd.file = 0;
}


/*
===============
SV_MVD_StopRecord_f
===============
*/
void SV_MVD_StopRecord_f( void ) {
	if ( !mvd.recording ) {
		Com_Printf( "MVD: Not recording.\n" );
		return;
	}

	SV_MVD_StopRecord();
}


/*
===============
SV_MVD_ConfigstringChanged
===============
*/
void SV_MVD_ConfigstringChanged( int index ) {
	if ( index >= 0 && index < MAX_CONFIGSTRINGS ) {
		mvd.csChanged[index] = qtrue;
	}
}


/*
===============
SV_MVD_CaptureServerCommand
===============
*/
void SV_MVD_CaptureServerCommand( int target, const char *cmd ) {
	int len;

	if ( mvd.cmdCount >= MAX_MVD_CMDS ) {
		return;
	}

	len = (int)strlen( cmd );
	if ( mvd.cmdBufUsed + len > MAX_MVD_CMDBUF ) {
		return;
	}

	mvd.cmds[mvd.cmdCount].target = target;
	mvd.cmds[mvd.cmdCount].offset = mvd.cmdBufUsed;
	mvd.cmds[mvd.cmdCount].len = len;
	Com_Memcpy( mvd.cmdBuf + mvd.cmdBufUsed, cmd, len );
	mvd.cmdBufUsed += len;
	mvd.cmdCount++;
}


/*
===============
SV_MVD_AutoStart
===============
*/
void SV_MVD_AutoStart( void ) {
	char defaultName[MAX_QPATH];
	time_t now;
	struct tm *tm_info;

	if ( !sv_mvdauto->integer || mvd.recording ) {
		return;
	}

	now = time( NULL );
	tm_info = localtime( &now );
	strftime( defaultName, sizeof( defaultName ), "mvd_%Y%m%d_%H%M%S", tm_info );

	SV_MVD_StartRecord( defaultName );
}
