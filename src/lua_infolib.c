// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2012-2016 by John "JTE" Muniz.
// Copyright (C) 2012-2018 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  lua_infolib.c
/// \brief infotable editing library for Lua scripting

#include "doomdef.h"
#include "fastcmp.h"
#include "info.h"
#include "dehacked.h"
#include "p_mobj.h"
#include "p_local.h"
#include "z_zone.h"

#include "lua_script.h"
#include "lua_libs.h"
#include "lua_hud.h" // hud_running errors
#include "lua_hook.h"	// cmd errors

boolean LUA_CallAction(enum actionnum actionnum, mobj_t *actor);
state_t *astate;

boolean actionsoverridden[NUMACTIONS] = {false};

//
// Sprite Names
//

// push sprite name
static int lib_getSprname(lua_State *L)
{
	UINT32 i;

	lua_remove(L, 1); // don't care about sprnames[] dummy userdata.

	if (lua_isnumber(L, 1))
	{
		i = lua_tonumber(L, 1);
		if (i > NUMSPRITES)
			return 0;
		lua_pushlstring(L, sprnames[i], 4);
		return 1;
	}
	else if (lua_isstring(L, 1))
	{
		const char *name = lua_tostring(L, 1);
		for (i = 0; i < NUMSPRITES; i++)
			if (fastcmp(name, sprnames[i]))
			{
				lua_pushinteger(L, i);
				return 1;
			}
	}
	return 0;
}

/// \todo Maybe make it tally up the used_spr from dehacked?
static int lib_sprnamelen(lua_State *L)
{
	lua_pushinteger(L, NUMSPRITES);
	return 1;
}

////////////////
// STATE INFO //
////////////////

// Uses astate to determine which state is calling it
// Then looks up which Lua action is assigned to that state and calls it
static void A_Lua(mobj_t *actor)
{
	boolean found = false;
	I_Assert(actor != NULL);

	lua_settop(gL, 0); // Just in case...
	lua_pushcfunction(gL, LUA_GetErrorMessage);

	// get the action for this state
	lua_getfield(gL, LUA_REGISTRYINDEX, LREG_STATEACTION);
	I_Assert(lua_istable(gL, -1));
	lua_pushlightuserdata(gL, astate);
	lua_rawget(gL, -2);
	I_Assert(lua_isfunction(gL, -1));
	lua_remove(gL, -2); // pop LREG_STATEACTION

	// get the name for this action, if possible.
	lua_getfield(gL, LUA_REGISTRYINDEX, LREG_ACTIONS);
	lua_pushnil(gL);
	while (lua_next(gL, -2))
	{
		if (lua_rawequal(gL, -1, -4))
		{
			found = true;
			superactions[superstack] = lua_tostring(gL, -2); // "A_ACTION"
			++superstack;
			lua_pop(gL, 2); // pop the name and function
			break;
		}
		lua_pop(gL, 1);
	}
	lua_pop(gL, 1); // pop LREG_ACTION

	LUA_PushUserdata(gL, actor, META_MOBJ);
	lua_pushinteger(gL, var1);
	lua_pushinteger(gL, var2);
	LUA_Call(gL, 3, 0, 1);

	if (found)
	{
		--superstack;
		superactions[superstack] = NULL;
	}
}

// Arbitrary states[] table index -> state_t *
static int lib_getState(lua_State *L)
{
	UINT32 i;
	lua_remove(L, 1);

	i = luaL_checkinteger(L, 1);
	if (i >= NUMSTATES)
		return luaL_error(L, "states[] index %d out of range (0 - %d)", i, NUMSTATES-1);
	LUA_PushUserdata(L, &states[i], META_STATE);
	return 1;
}

// Lua table full of data -> states[] (set the values all at once! :D :D)
static int lib_setState(lua_State *L)
{
	state_t *state;
	lua_remove(L, 1); // don't care about states[] userdata.
	{
		UINT32 i = luaL_checkinteger(L, 1);
		if (i >= NUMSTATES)
			return luaL_error(L, "states[] index %d out of range (0 - %d)", i, NUMSTATES-1);
		state = &states[i]; // get the state to assign to.
	}
	luaL_checktype(L, 2, LUA_TTABLE); // check that we've been passed a table.
	lua_remove(L, 1); // pop state num, don't need it any more.
	lua_settop(L, 1); // cut the stack here. the only thing left now is the table of data we're assigning to the state.

	if (hud_running)
		return luaL_error(L, "Do not alter states in HUD rendering code!");
	if (hook_cmd_running)
		return luaL_error(L, "Do not alter states in BuildCMD code!");

	// clear the state to start with, in case of missing table elements
	memset(state,0,sizeof(state_t));
	state->tics = -1;

	lua_pushnil(L);
	while (lua_next(L, 1)) {
		lua_Integer i = 0;
		const char *str = NULL;
		lua_Integer value;
		if (lua_isnumber(L, 2))
			i = lua_tointeger(L, 2);
		else
			str = luaL_checkstring(L, 2);

		if (i == 1 || (str && fastcmp(str, "sprite"))) {
			value = luaL_checkinteger(L, 3);
			if (value < SPR_NULL || value >= NUMSPRITES)
				return luaL_error(L, "sprite number %d is invalid.", value);
			state->sprite = (spritenum_t)value;
		} else if (i == 2 || (str && fastcmp(str, "frame"))) {
			state->frame = (UINT32)luaL_checkinteger(L, 3);
		} else if (i == 3 || (str && fastcmp(str, "tics"))) {
			state->tics = (INT32)luaL_checkinteger(L, 3);
		} else if (i == 4 || (str && fastcmp(str, "action"))) {
			switch(lua_type(L, 3))
			{
			case LUA_TNIL: // Null? Set the action to nothing, then.
				state->action.acp1 = NULL;
				break;
			case LUA_TSTRING: // It's a string, expect the name of a built-in action
				LUA_SetActionByName(state, lua_tostring(L, 3));
				break;
			case LUA_TFUNCTION: // It's a function (a Lua function or a C function? either way!)
				lua_getfield(L, LUA_REGISTRYINDEX, LREG_STATEACTION);
				I_Assert(lua_istable(L, -1));
				lua_pushlightuserdata(L, state); // We'll store this function by the state's pointer in the registry.
				lua_pushvalue(L, 3); // Bring it to the top of the stack
				lua_rawset(L, -3); // Set it in the registry
				lua_pop(L, 1); // pop LREG_STATEACTION
				state->action.acp1 = (actionf_p1)A_Lua; // Set the action for the userdata.
				break;
			default: // ?!
				return luaL_typerror(L, 3, "function");
			}
		} else if (i == 5 || (str && fastcmp(str, "var1"))) {
			state->var1 = (INT32)luaL_checkinteger(L, 3);
		} else if (i == 6 || (str && fastcmp(str, "var2"))) {
			state->var2 = (INT32)luaL_checkinteger(L, 3);
		} else if (i == 7 || (str && fastcmp(str, "nextstate"))) {
			value = luaL_checkinteger(L, 3);
			if (value < S_NULL || value >= NUMSTATES)
				return luaL_error(L, "nextstate number %d is invalid.", value);
			state->nextstate = (statenum_t)value;
		}
		lua_pop(L, 1);
	}
	return 0;
}

// #states -> NUMSTATES
static int lib_statelen(lua_State *L)
{
	lua_pushinteger(L, NUMSTATES);
	return 1;
}

boolean LUA_SetLuaAction(void *stv, const char *action)
{
	state_t *st = (state_t *)stv;

	I_Assert(st != NULL);
	//I_Assert(st >= states && st < states+NUMSTATES); // if you REALLY want to be paranoid...
	I_Assert(action != NULL);

	if (!gL) // Lua isn't loaded,
		return false; // action not set.

	// action is assumed to be in all-caps already !!
	// the registry is case-sensitive, so we strupr everything that enters it.

	lua_getfield(gL, LUA_REGISTRYINDEX, LREG_ACTIONS);
	lua_getfield(gL, -1, action);

	if (lua_isnil(gL, -1)) // no match
	{
		lua_pop(gL, 2); // pop nil and LREG_ACTIONS
		return false; // action not set.
	}

	lua_getfield(gL, LUA_REGISTRYINDEX, LREG_STATEACTION);
	I_Assert(lua_istable(gL, -1));
	lua_pushlightuserdata(gL, stv); // We'll store this function by the state's pointer in the registry.
	lua_pushvalue(gL, -3); // Bring it to the top of the stack
	lua_rawset(gL, -3); // Set it in the registry
	lua_pop(gL, 1); // pop LREG_STATEACTION

	lua_pop(gL, 2); // pop the function and LREG_ACTIONS
	st->action.acp1 = (actionf_p1)A_Lua; // Set the action for the userdata.
	return true; // action successfully set.
}

boolean LUA_CallAction(enum actionnum actionnum, mobj_t *actor)
{
	I_Assert(actor != NULL);

	if (!actionsoverridden[actionnum]) // The action is not overriden,
		return false; // action not called.

	if (superstack && fasticmp(actionpointers[actionnum].name, superactions[superstack-1])) // the action is calling itself,
		return false; // let it call the hardcoded function 
		
	lua_pushcfunction(gL, LUA_GetErrorMessage);

	// grab function by uppercase name.
	lua_getfield(gL, LUA_REGISTRYINDEX, LREG_ACTIONS);
	lua_getfield(gL, -1, actionpointers[actionnum].name);
	lua_remove(gL, -2); // pop LREG_ACTIONS

	if (lua_isnil(gL, -1)) // no match
	{
		lua_pop(gL, 2); // pop nil and error handler
		return false; // action not called.
	}

	if (superstack == MAXRECURSION)
	{
		CONS_Alert(CONS_WARNING, "Max Lua Action recursion reached! Cool it on the calling A_Action functions from inside A_Action functions!\n");
		lua_pop(gL, 2); // pop function and error handler
		return true;
	}

	// Found a function.
	// Call it with (actor, var1, var2)
	I_Assert(lua_isfunction(gL, -1));
	LUA_PushUserdata(gL, actor, META_MOBJ);
	lua_pushinteger(gL, var1);
	lua_pushinteger(gL, var2);

	superactions[superstack] = actionpointers[actionnum].name;
	++superstack;

	LUA_Call(gL, 3, 0, -(2 + 3));
	lua_pop(gL, -1); // Error handler

	--superstack;
	superactions[superstack] = NULL;
	return true; // action successfully called.
}

enum state_e {
	state_sprite = 0,
	state_frame,
	state_tics,
	state_action,
	state_var1,
	state_var2,
	state_nextstate,
};

const char *const state_opt[] = {
	"sprite",
	"frame",
	"tics",
	"action",
	"var1",
	"var2",
	"nextstate",
	NULL
};

static int state_fields_ref = LUA_NOREF;

// state_t *, field -> number
static int state_get(lua_State *L)
{
	state_t *st = *((state_t **)luaL_checkudata(L, 1, META_STATE));
	enum state_e field = Lua_optoption(L, 2, -1, state_fields_ref);
	lua_Integer number;

	switch (field)
	{
		case state_sprite:
			number = st->sprite;
		break;

		case state_frame:
			number = st->frame;
		break;

		case state_tics:
			number = st->tics;
		break;

		case state_action:
		{
			const char *name;
			if (!st->action.acp1) // Action is NULL.
				return 0; // return nil.
			if (st->action.acp1 == (actionf_p1)A_Lua) { // This is a Lua function?
				lua_getfield(L, LUA_REGISTRYINDEX, LREG_STATEACTION);
				I_Assert(lua_istable(L, -1));
				lua_pushlightuserdata(L, st); // Push the state pointer and
				lua_rawget(L, -2); // use it to get the actual Lua function.
				lua_remove(L, -2); // pop LREG_STATEACTION
				return 1; // Return the Lua function.
			}
			name = LUA_GetActionName(&st->action); // find a hardcoded function name
			if (!name) // If it's not a hardcoded function and it's not a Lua function...
				return 0; // Just what is this??
			// get the function from the global
			// because the metatable will trigger.
			lua_getglobal(L, name); // actually gets from LREG_ACTIONS if applicable, and pushes a new C closure if not.
			lua_pushstring(L, name); // push the name we found.
			return 2; // return both the function and its name, in case somebody wanted to do a comparison by name or something?
		}
		break;

		case state_var1:
			number = st->var1;
		break;

		case state_var2:
			number = st->var2;
		break;

		case state_nextstate:
			number = st->nextstate;
		break;

		default:
		{
			if (devparm)
				return luaL_error(L, LUA_QL("state_t") " has no field named " LUA_QS, field);
			return 0;
		}
	}

	lua_pushinteger(L, number);
	return 1;
}

// state_t *, field, number -> states[]
static int state_set(lua_State *L)
{
	state_t *st = *((state_t **)luaL_checkudata(L, 1, META_STATE));
	enum state_e field = Lua_optoption(L, 2, -1, state_fields_ref);
	lua_Integer value;

	if (hud_running)
		return luaL_error(L, "Do not alter states in HUD rendering code!");
	if (hook_cmd_running)
		return luaL_error(L, "Do not alter states in BuildCMD code!");

	switch (field)
	{
	case state_sprite:
		value = luaL_checknumber(L, 3);
		if (value < SPR_NULL || value >= NUMSPRITES)
			return luaL_error(L, "sprite number %d is invalid.", value);
		st->sprite = (spritenum_t)value;
	break;

	case state_frame:
		st->frame = (UINT32)luaL_checknumber(L, 3);
	break;

	case state_tics:
		st->tics = (INT32)luaL_checknumber(L, 3);
	break;

	case state_action:
		switch(lua_type(L, 3))
		{
		case LUA_TNIL: // Null? Set the action to nothing, then.
			st->action.acp1 = NULL;
			break;
		case LUA_TSTRING: // It's a string, expect the name of a built-in action
			LUA_SetActionByName(st, lua_tostring(L, 3));
			break;
		case LUA_TFUNCTION: // It's a function (a Lua function or a C function? either way!)
			lua_getfield(L, LUA_REGISTRYINDEX, LREG_STATEACTION);
			I_Assert(lua_istable(L, -1));
			lua_pushlightuserdata(L, st); // We'll store this function by the state's pointer in the registry.
			lua_pushvalue(L, 3); // Bring it to the top of the stack
			lua_rawset(L, -3); // Set it in the registry
			lua_pop(L, 1); // pop LREG_STATEACTION
			st->action.acp1 = (actionf_p1)A_Lua; // Set the action for the userdata.
			break;
		default: // ?!
			return luaL_typerror(L, 3, "function");
		}
	break;

	case state_var1:
		st->var1 = (INT32)luaL_checknumber(L, 3);
	break;

	case state_var2:
		st->var2 = (INT32)luaL_checknumber(L, 3);
	break;

	case state_nextstate:
		value = luaL_checkinteger(L, 3);
		if (value < S_NULL || value >= NUMSTATES)
			return luaL_error(L, "nextstate number %d is invalid.", value);
		st->nextstate = (statenum_t)value;
	break;

	default:
		return luaL_error(L, LUA_QL("state_t") " has no field named " LUA_QS, field);
	}

	return 0;
}

// state_t * -> S_*
static int state_num(lua_State *L)
{
	state_t *state = *((state_t **)luaL_checkudata(L, 1, META_STATE));
	lua_pushinteger(L, state-states);
	return 1;
}

///////////////
// MOBJ INFO //
///////////////

// Arbitrary mobjinfo[] table index -> mobjinfo_t *
static int lib_getMobjInfo(lua_State *L)
{
	UINT32 i;
	lua_remove(L, 1);

	i = luaL_checkinteger(L, 1);
	if (i >= NUMMOBJTYPES)
		return luaL_error(L, "mobjinfo[] index %d out of range (0 - %d)", i, NUMMOBJTYPES-1);
	LUA_PushUserdata(L, &mobjinfo[i], META_MOBJINFO);
	return 1;
}

// Lua table full of data -> mobjinfo[]
static int lib_setMobjInfo(lua_State *L)
{
	mobjinfo_t *info;
	lua_remove(L, 1); // don't care about mobjinfo[] userdata.
	{
		UINT32 i = luaL_checkinteger(L, 1);
		if (i >= NUMMOBJTYPES)
			return luaL_error(L, "mobjinfo[] index %d out of range (0 - %d)", i, NUMMOBJTYPES-1);
		info = &mobjinfo[i]; // get the mobjinfo to assign to.
	}
	luaL_checktype(L, 2, LUA_TTABLE); // check that we've been passed a table.
	lua_remove(L, 1); // pop mobjtype num, don't need it any more.
	lua_settop(L, 1); // cut the stack here. the only thing left now is the table of data we're assigning to the mobjinfo.

	if (hud_running)
		return luaL_error(L, "Do not alter mobjinfo in HUD rendering code!");
	if (hook_cmd_running)
		return luaL_error(L, "Do not alter mobjinfo in BuildCMD code!");

	// clear the mobjinfo to start with, in case of missing table elements
	memset(info,0,sizeof(mobjinfo_t));
	info->doomednum = -1; // default to no editor value
	info->spawnhealth = 1; // avoid 'dead' noclip behaviors

	lua_pushnil(L);
	while (lua_next(L, 1)) {
		lua_Integer i = 0;
		const char *str = NULL;
		lua_Integer value;
		if (lua_isnumber(L, 2))
			i = lua_tointeger(L, 2);
		else
			str = luaL_checkstring(L, 2);

		if (i == 1 || (str && fastcmp(str,"doomednum")))
			info->doomednum = (INT32)luaL_checkinteger(L, 3);
		else if (i == 2 || (str && fastcmp(str,"spawnstate"))) {
			value = luaL_checkinteger(L, 3);
			if (value < S_NULL || value >= NUMSTATES)
				return luaL_error(L, "spawnstate number %d is invalid.", value);
			info->spawnstate = (statenum_t)value;
		} else if (i == 3 || (str && fastcmp(str,"spawnhealth")))
			info->spawnhealth = (INT32)luaL_checkinteger(L, 3);
		else if (i == 4 || (str && fastcmp(str,"seestate"))) {
			value = luaL_checkinteger(L, 3);
			if (value < S_NULL || value >= NUMSTATES)
				return luaL_error(L, "seestate number %d is invalid.", value);
			info->seestate = (statenum_t)value;
		} else if (i == 5 || (str && fastcmp(str,"seesound"))) {
			value = luaL_checkinteger(L, 3);
			if (value < sfx_None || value >= NUMSFX)
				return luaL_error(L, "seesound number %d is invalid.", value);
			info->seesound = (sfxenum_t)value;
		} else if (i == 6 || (str && fastcmp(str,"reactiontime")))
			info->reactiontime = (INT32)luaL_checkinteger(L, 3);
		else if (i == 7 || (str && fastcmp(str,"attacksound")))
			info->attacksound = luaL_checkinteger(L, 3);
		else if (i == 8 || (str && fastcmp(str,"painstate")))
			info->painstate = luaL_checkinteger(L, 3);
		else if (i == 9 || (str && fastcmp(str,"painchance")))
			info->painchance = (INT32)luaL_checkinteger(L, 3);
		else if (i == 10 || (str && fastcmp(str,"painsound")))
			info->painsound = luaL_checkinteger(L, 3);
		else if (i == 11 || (str && fastcmp(str,"meleestate")))
			info->meleestate = luaL_checkinteger(L, 3);
		else if (i == 12 || (str && fastcmp(str,"missilestate")))
			info->missilestate = luaL_checkinteger(L, 3);
		else if (i == 13 || (str && fastcmp(str,"deathstate")))
			info->deathstate = luaL_checkinteger(L, 3);
		else if (i == 14 || (str && fastcmp(str,"xdeathstate")))
			info->xdeathstate = luaL_checkinteger(L, 3);
		else if (i == 15 || (str && fastcmp(str,"deathsound")))
			info->deathsound = luaL_checkinteger(L, 3);
		else if (i == 16 || (str && fastcmp(str,"speed")))
			info->speed = luaL_checkfixed(L, 3);
		else if (i == 17 || (str && fastcmp(str,"radius")))
			info->radius = luaL_checkfixed(L, 3);
		else if (i == 18 || (str && fastcmp(str,"height")))
			info->height = luaL_checkfixed(L, 3);
		else if (i == 19 || (str && fastcmp(str,"dispoffset")))
			info->dispoffset = (INT32)luaL_checkinteger(L, 3);
		else if (i == 20 || (str && fastcmp(str,"mass")))
			info->mass = (INT32)luaL_checkinteger(L, 3);
		else if (i == 21 || (str && fastcmp(str,"damage")))
			info->damage = (INT32)luaL_checkinteger(L, 3);
		else if (i == 22 || (str && fastcmp(str,"activesound")))
			info->activesound = luaL_checkinteger(L, 3);
		else if (i == 23 || (str && fastcmp(str,"flags")))
			info->flags = (INT32)luaL_checkinteger(L, 3);
		else if (i == 24 || (str && fastcmp(str,"raisestate"))) {
			info->raisestate = luaL_checkinteger(L, 3);
		}
		lua_pop(L, 1);
	}
	return 0;
}

// #mobjinfo -> NUMMOBJTYPES
static int lib_mobjinfolen(lua_State *L)
{
	lua_pushinteger(L, NUMMOBJTYPES);
	return 1;
}

enum mobjinfo_e
{
	mobjinfo_doomednum,
	mobjinfo_spawnstate,
	mobjinfo_spawnhealth,
	mobjinfo_seestate,
	mobjinfo_seesound,
	mobjinfo_reactiontime,
	mobjinfo_attacksound,
	mobjinfo_painstate,
	mobjinfo_painchance,
	mobjinfo_painsound,
	mobjinfo_meleestate,
	mobjinfo_missilestate,
	mobjinfo_deathstate,
	mobjinfo_xdeathstate,
	mobjinfo_deathsound,
	mobjinfo_speed,
	mobjinfo_radius,
	mobjinfo_height,
	mobjinfo_dispoffset,
	mobjinfo_mass,
	mobjinfo_damage,
	mobjinfo_activesound,
	mobjinfo_flags,
	mobjinfo_raisestate,
};

const char *const mobjinfo_opt[] = {
	"doomednum",
	"spawnstate",
	"spawnhealth",
	"seestate",
	"seesound",
	"reactiontime",
	"attacksound",
	"painstate",
	"painchance",
	"painsound",
	"meleestate",
	"missilestate",
	"deathstate",
	"xdeathstate",
	"deathsound",
	"speed",
	"radius",
	"height",
	"dispoffset",
	"mass",
	"damage",
	"activesound",
	"flags",
	"raisestate",
	NULL,
};

static int mobjinfo_fields_ref = LUA_NOREF;

// mobjinfo_t *, field -> number
static int mobjinfo_get(lua_State *L)
{
	mobjinfo_t *info = *((mobjinfo_t **)luaL_checkudata(L, 1, META_MOBJINFO));
	enum mobjinfo_e field = Lua_optoption(L, 2, -1, mobjinfo_fields_ref);

	I_Assert(info != NULL);
	I_Assert(info >= mobjinfo);

	switch (field)
	{
	case mobjinfo_doomednum:
		lua_pushinteger(L, info->doomednum);
		break;
	case mobjinfo_spawnstate:
		lua_pushinteger(L, info->spawnstate);
		break;
	case mobjinfo_spawnhealth:
		lua_pushinteger(L, info->spawnhealth);
		break;
	case mobjinfo_seestate:
		lua_pushinteger(L, info->seestate);
		break;
	case mobjinfo_seesound:
		lua_pushinteger(L, info->seesound);
		break;
	case mobjinfo_reactiontime:
		lua_pushinteger(L, info->reactiontime);
		break;
	case mobjinfo_attacksound:
		lua_pushinteger(L, info->attacksound);
		break;
	case mobjinfo_painstate:
		lua_pushinteger(L, info->painstate);
		break;
	case mobjinfo_painchance:
		lua_pushinteger(L, info->painchance);
		break;
	case mobjinfo_painsound:
		lua_pushinteger(L, info->painsound);
		break;
	case mobjinfo_meleestate:
		lua_pushinteger(L, info->meleestate);
		break;
	case mobjinfo_missilestate:
		lua_pushinteger(L, info->missilestate);
		break;
	case mobjinfo_deathstate:
		lua_pushinteger(L, info->deathstate);
		break;
	case mobjinfo_xdeathstate:
		lua_pushinteger(L, info->xdeathstate);
		break;
	case mobjinfo_deathsound:
		lua_pushinteger(L, info->deathsound);
		break;
	case mobjinfo_speed:
		lua_pushinteger(L, info->speed); // sometimes it's fixed_t, sometimes it's not...
		break;
	case mobjinfo_radius:
		lua_pushfixed(L, info->radius);
		break;
	case mobjinfo_height:
		lua_pushfixed(L, info->height);
		break;
	case mobjinfo_dispoffset:
		lua_pushinteger(L, info->dispoffset);
		break;
	case mobjinfo_mass:
		lua_pushinteger(L, info->mass);
		break;
	case mobjinfo_damage:
		lua_pushinteger(L, info->damage);
		break;
	case mobjinfo_activesound:
		lua_pushinteger(L, info->activesound);
		break;
	case mobjinfo_flags:
		lua_pushinteger(L, info->flags);
		break;
	case mobjinfo_raisestate:
		lua_pushinteger(L, info->raisestate);
		break;
	default:
		lua_getfield(L, LUA_REGISTRYINDEX, LREG_EXTVARS);
		I_Assert(lua_istable(L, -1));
		lua_pushlightuserdata(L, info);
		lua_rawget(L, -2);
		if (!lua_istable(L, -1)) { // no extra values table
			CONS_Debug(DBG_LUA, M_GetText("'%s' has no field named '%s'; returning nil.\n"), "mobjinfo_t", lua_tostring(L, 2));
			return 0;
		}
		lua_pushvalue(L, 2); // field name
		lua_gettable(L, -2);
		if (lua_isnil(L, -1)) // no value for this field
			CONS_Debug(DBG_LUA, M_GetText("'%s' has no field named '%s'; returning nil.\n"), "mobjinfo_t", lua_tostring(L, 2));
		break;
	}
	return 1;
}

// mobjinfo_t *, field, number -> mobjinfo[]
static int mobjinfo_set(lua_State *L)
{
	mobjinfo_t *info = *((mobjinfo_t **)luaL_checkudata(L, 1, META_MOBJINFO));
	enum mobjinfo_e field = Lua_optoption(L, 2, -1, mobjinfo_fields_ref);

	if (hud_running)
		return luaL_error(L, "Do not alter mobjinfo in HUD rendering code!");
	if (hook_cmd_running)
		return luaL_error(L, "Do not alter mobjinfo in BuildCMD code!");

	I_Assert(info != NULL);
	I_Assert(info >= mobjinfo);

	switch (field)
	{
	case mobjinfo_doomednum:
		info->doomednum = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_spawnstate:
		info->spawnstate = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_spawnhealth:
		info->spawnhealth = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_seestate:
		info->seestate = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_seesound:
		info->seesound = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_reactiontime:
		info->reactiontime = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_attacksound:
		info->attacksound = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_painstate:
		info->painstate = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_painchance:
		info->painchance = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_painsound:
		info->painsound = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_meleestate:
		info->meleestate = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_missilestate:
		info->missilestate = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_deathstate:
		info->deathstate = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_xdeathstate:
		info->xdeathstate = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_deathsound:
		info->deathsound = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_speed:
		info->speed = luaL_checkfixed(L, 3);
		break;
	case mobjinfo_radius:
		info->radius = luaL_checkfixed(L, 3);
		break;
	case mobjinfo_height:
		info->height = luaL_checkfixed(L, 3);
		break;
	case mobjinfo_dispoffset:
		info->dispoffset = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_mass:
		info->mass = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_damage:
		info->damage = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_activesound:
		info->activesound = luaL_checkinteger(L, 3);
		break;
	case mobjinfo_flags:
		info->flags = (INT32)luaL_checkinteger(L, 3);
		break;
	case mobjinfo_raisestate:
		info->raisestate = luaL_checkinteger(L, 3);
		break;
	default:
		lua_getfield(L, LUA_REGISTRYINDEX, LREG_EXTVARS);
		I_Assert(lua_istable(L, -1));
		lua_pushlightuserdata(L, info);
		lua_rawget(L, -2);
		if (lua_isnil(L, -1)) {
			// This index doesn't have a table for extra values yet, let's make one.
			lua_pop(L, 1);
			CONS_Debug(DBG_LUA, M_GetText("'%s' has no field named '%s'; adding it as Lua data.\n"), "mobjinfo_t", lua_tostring(L, 2));
			lua_newtable(L);
			lua_pushlightuserdata(L, info);
			lua_pushvalue(L, -2); // ext value table
			lua_rawset(L, -4); // LREG_EXTVARS table
		}
		lua_pushvalue(L, 2); // key
		lua_pushvalue(L, 3); // value to store
		lua_settable(L, -3);
		lua_pop(L, 2);
	}
	return 0;
}

// mobjinfo_t * -> MT_*
static int mobjinfo_num(lua_State *L)
{
	mobjinfo_t *info = *((mobjinfo_t **)luaL_checkudata(L, 1, META_MOBJINFO));

	I_Assert(info != NULL);
	I_Assert(info >= mobjinfo);

	lua_pushinteger(L, info-mobjinfo);
	return 1;
}

//////////////
// SFX INFO //
//////////////

// Arbitrary S_sfx[] table index -> sfxinfo_t *
static int lib_getSfxInfo(lua_State *L)
{
	UINT32 i;
	lua_remove(L, 1);

	i = luaL_checkinteger(L, 1);
	if (i >= NUMSFX)
		return luaL_error(L, "sfxinfo[] index %d out of range (0 - %d)", i, NUMSFX-1);
	LUA_PushUserdata(L, &S_sfx[i], META_SFXINFO);
	return 1;
}

enum sfxinfo_e {
	sfxinfo_name = 0,
	sfxinfo_singular,
	sfxinfo_priority,
	sfxinfo_flags, // "pitch"
	sfxinfo_skinsound
};

const char *const sfxinfo_opt[] = {
	"name",
	"singular",
	"priority",
	"flags",
	"skinsound",
	NULL
};

static int sfxinfo_fields_ref = LUA_NOREF;

// stack: dummy, S_sfx[] table index, table of values to set.
static int lib_setSfxInfo(lua_State *L)
{
	sfxinfo_t *info;

	lua_remove(L, 1);
	{
		UINT32 i = luaL_checkinteger(L, 1);
		if (i >= NUMSFX)
			return luaL_error(L, "sfxinfo[] index %d out of range (0 - %d)", i, NUMSFX-1);
		info = &S_sfx[i]; // get the mobjinfo to assign to.
	}
	luaL_checktype(L, 2, LUA_TTABLE); // check that we've been passed a table.
	lua_remove(L, 1); // pop mobjtype num, don't need it any more.
	lua_settop(L, 1); // cut the stack here. the only thing left now is the table of data we're assigning to the mobjinfo.

	if (hud_running)
		return luaL_error(L, "Do not alter sfxinfo in HUD rendering code!");
	if (hook_cmd_running)
		return luaL_error(L, "Do not alter sfxinfo in BuildCMD code!");

	lua_pushnil(L);
	while (lua_next(L, 1)) {
		enum sfxinfo_e i;

		if (lua_isnumber(L, 2))
		{
			int j = lua_tointeger(L, 2) - 1;

			// Read and Write enums were combined, need to do this switch now
			switch (j)
			{
				case 1:
					i = sfxinfo_singular;
				break;

				case 2:
					i = sfxinfo_priority;
				break;

				case 3:
					i = sfxinfo_flags;
				break;

				default:
					i = -1;
				break;
			}
		}
		else
			i = Lua_optoption(L, 2, -1, sfxinfo_fields_ref);

		switch(i)
		{
		case sfxinfo_singular:
			info->singularity = luaL_checkboolean(L, 3);
			break;
		case sfxinfo_priority:
			info->priority = (INT32)luaL_checkinteger(L, 3);
			break;
		case sfxinfo_flags:
			info->pitch = (INT32)luaL_checkinteger(L, 3);
			break;
		default:
			break;
		}
		lua_pop(L, 1);
	}

	return 0;
}

static int lib_sfxlen(lua_State *L)
{
	lua_pushinteger(L, NUMSFX);
	return 1;
}

// sfxinfo_t *, field
static int sfxinfo_get(lua_State *L)
{
	sfxinfo_t *sfx = *((sfxinfo_t **)luaL_checkudata(L, 1, META_SFXINFO));
	enum sfxinfo_e field = Lua_optoption(L, 2, -1, sfxinfo_fields_ref);

	I_Assert(sfx != NULL);

	switch (field)
	{
	case sfxinfo_name:
		lua_pushstring(L, sfx->name);
		return 1;
	case sfxinfo_singular:
		lua_pushboolean(L, sfx->singularity);
		return 1;
	case sfxinfo_priority:
		lua_pushinteger(L, sfx->priority);
		return 1;
	case sfxinfo_flags:
		lua_pushinteger(L, sfx->pitch);
		return 1;
	case sfxinfo_skinsound:
		lua_pushinteger(L, sfx->skinsound);
		return 1;
	default:
		return luaL_error(L, LUA_QL("sfxinfo_t") " has no field named " LUA_QS, lua_tostring(L, 2));
	}
	return 0;
}

// sfxinfo_t *, field, value
static int sfxinfo_set(lua_State *L)
{
	sfxinfo_t *sfx = *((sfxinfo_t **)luaL_checkudata(L, 1, META_SFXINFO));
	int field = Lua_optoption(L, 2, -1, sfxinfo_fields_ref);

	if (hud_running)
		return luaL_error(L, "Do not alter S_sfx in HUD rendering code!");
	if (hook_cmd_running)
		return luaL_error(L, "Do not alter S_sfx in BuildCMD code!");

	I_Assert(sfx != NULL);

	lua_remove(L, 1); // remove sfxinfo
	lua_remove(L, 1); // remove field
	lua_settop(L, 1); // leave only one value

	switch (field)
	{
	case sfxinfo_singular:
		sfx->singularity = luaL_checkboolean(L, 1);
		break;
	case sfxinfo_priority:
		sfx->priority = luaL_checkinteger(L, 1);
		break;
	case sfxinfo_flags:
		sfx->pitch = luaL_checkinteger(L, 1);
		break;
	default:
		if (field == -1)
			return luaL_error(L, LUA_QL("sfxinfo_t") " has no field named " LUA_QS, lua_tostring(L, 2));
		else
			return luaL_error(L, LUA_QL("sfxinfo_t") " field " LUA_QS " is read-only", lua_tostring(L, 2));
	}
	return 0;
}

static int sfxinfo_num(lua_State *L)
{
	sfxinfo_t *sfx = *((sfxinfo_t **)luaL_checkudata(L, 1, META_SFXINFO));

	I_Assert(sfx != NULL);
	I_Assert(sfx >= S_sfx);

	lua_pushinteger(L, (UINT32)(sfx-S_sfx));
	return 1;
}

//////////////////////////////
//
// Now push all these functions into the Lua state!
//
//
int LUA_InfoLib(lua_State *L)
{
	// index of A_Lua actions to run for each state
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, LREG_STATEACTION);

	// index of globally available Lua actions by function name
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, LREG_ACTIONS);

	luaL_newmetatable(L, META_STATE);
		lua_pushcfunction(L, state_get);
		lua_setfield(L, -2, "__index");

		lua_pushcfunction(L, state_set);
		lua_setfield(L, -2, "__newindex");

		lua_pushcfunction(L, state_num);
		lua_setfield(L, -2, "__len");
	lua_pop(L, 1);

	state_fields_ref = Lua_CreateFieldTable(L, state_opt);

	luaL_newmetatable(L, META_MOBJINFO);
		lua_pushcfunction(L, mobjinfo_get);
		lua_setfield(L, -2, "__index");

		lua_pushcfunction(L, mobjinfo_set);
		lua_setfield(L, -2, "__newindex");

		lua_pushcfunction(L, mobjinfo_num);
		lua_setfield(L, -2, "__len");
	lua_pop(L, 1);

	mobjinfo_fields_ref = Lua_CreateFieldTable(L, mobjinfo_opt);

	luaL_newmetatable(L, META_SFXINFO);
		lua_pushcfunction(L, sfxinfo_get);
		lua_setfield(L, -2, "__index");

		lua_pushcfunction(L, sfxinfo_set);
		lua_setfield(L, -2, "__newindex");

		lua_pushcfunction(L, sfxinfo_num);
		lua_setfield(L, -2, "__len");
	lua_pop(L, 1);

	sfxinfo_fields_ref = Lua_CreateFieldTable(L, sfxinfo_opt);

	lua_newuserdata(L, 0);
		lua_createtable(L, 0, 2);
			lua_pushcfunction(L, lib_getSprname);
			lua_setfield(L, -2, "__index");

			lua_pushcfunction(L, lib_sprnamelen);
			lua_setfield(L, -2, "__len");
		lua_setmetatable(L, -2);
	lua_setglobal(L, "sprnames");

	lua_newuserdata(L, 0);
		lua_createtable(L, 0, 2);
			lua_pushcfunction(L, lib_getState);
			lua_setfield(L, -2, "__index");

			lua_pushcfunction(L, lib_setState);
			lua_setfield(L, -2, "__newindex");

			lua_pushcfunction(L, lib_statelen);
			lua_setfield(L, -2, "__len");
		lua_setmetatable(L, -2);
	lua_setglobal(L, "states");

	lua_newuserdata(L, 0);
		lua_createtable(L, 0, 2);
			lua_pushcfunction(L, lib_getMobjInfo);
			lua_setfield(L, -2, "__index");

			lua_pushcfunction(L, lib_setMobjInfo);
			lua_setfield(L, -2, "__newindex");

			lua_pushcfunction(L, lib_mobjinfolen);
			lua_setfield(L, -2, "__len");
		lua_setmetatable(L, -2);
	lua_setglobal(L, "mobjinfo");

	lua_newuserdata(L, 0);
		lua_createtable(L, 0, 2);
			lua_pushcfunction(L, lib_getSfxInfo);
			lua_setfield(L, -2, "__index");

			lua_pushcfunction(L, lib_setSfxInfo);
			lua_setfield(L, -2, "__newindex");

			lua_pushcfunction(L, lib_sfxlen);
			lua_setfield(L, -2, "__len");
		lua_setmetatable(L, -2);
	lua_pushvalue(L, -1);
	lua_setglobal(L, "S_sfx");
	lua_setglobal(L, "sfxinfo");
	return 0;
}
