// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2004      by Stephen McGranahan
// Copyright (C) 2015-2018 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  p_slopes.c
/// \brief ZDoom + Eternity Engine Slopes, ported and enhanced by Kalaron

#include "doomdef.h"
#include "r_defs.h"
#include "r_state.h"
#include "m_bbox.h"
#include "z_zone.h"
#include "p_local.h"
#include "p_spec.h"
#include "p_slopes.h"
#include "p_setup.h"
#include "r_main.h"
#include "p_maputl.h"
#include "w_wad.h"
#include "r_fps.h"

pslope_t *slopelist = NULL;
UINT16 slopecount = 0;

thinker_t *dynthinklist;
size_t dynthinknum;

/// Links previously queued thinker list to the main thinker list.
void P_LinkSlopeThinkers (void)
{
	size_t i;
	thinker_t *th = dynthinklist;

	for (i = 0; i < dynthinknum; i++)
	{
		thinker_t *next = th->next;
		P_AddThinker(th);
		th = next;
	}
}

/// Queues a thinker to a partial linked list to be immediately incorporated later via P_LinkSlopeThinkers().
static void P_QueueSlopeThinker (thinker_t* th)
{
	thinker_t* last = dynthinklist;

	// First entry.
	if (!last)
	{
		dynthinklist = th;
		dynthinknum++;
		return;
	}

	while (last->next)
		last = last->next;

	last->next = th;

	dynthinknum++;
}

// Calculate line normal
void P_CalculateSlopeNormal(pslope_t *slope) {
	slope->normal.z = FINECOSINE(slope->zangle>>ANGLETOFINESHIFT);
	slope->normal.x = FixedMul(FINESINE(slope->zangle>>ANGLETOFINESHIFT), slope->d.x);
	slope->normal.y = FixedMul(FINESINE(slope->zangle>>ANGLETOFINESHIFT), slope->d.y);
}

/// Setup slope via 3 vertexes.
static void ReconfigureViaVertexes (pslope_t *slope, const vector3_t v1, const vector3_t v2, const vector3_t v3)
{
	vector3_t vec1, vec2;

	// Set origin.
	FV3_Copy(&slope->o, &v1);

	// Get slope's normal.
	FV3_SubEx(&v2, &v1, &vec1);
	FV3_SubEx(&v3, &v1, &vec2);

	// Set some defaults for a non-sloped "slope"
	if (vec1.z == 0 && vec2.z == 0)
	{
		/// \todo Fix fully flat cases.

		slope->zangle = slope->xydirection = 0;
		slope->zdelta = slope->d.x = slope->d.y = 0;
	}
	else
	{
		/// \note Using fixed point for vectorial products easily leads to overflows so we work around by downscaling them.
		fixed_t m = max(
			max(max(abs(vec1.x), abs(vec1.y)), abs(vec1.z)),
			max(max(abs(vec2.x), abs(vec2.y)), abs(vec2.z))
		) >> 5; // shifting right by 5 is good enough.

		FV3_Cross(
				FV3_Divide(&vec1, m),
				FV3_Divide(&vec2, m),
				&slope->normal
				);

		// NOTE: FV3_Magnitude() doesn't work properly in some cases, and chaining FixedHypot() seems to give worse results.
		m = R_PointToDist2(0, 0, R_PointToDist2(0, 0, slope->normal.x, slope->normal.y), slope->normal.z);

		// Invert normal if it's facing down.
		if (slope->normal.z < 0)
			m = -m;

		FV3_Divide(&slope->normal, m);

		// Get direction vector
		m = FixedHypot(slope->normal.x, slope->normal.y);
		slope->d.x = -FixedDiv(slope->normal.x, m);
		slope->d.y = -FixedDiv(slope->normal.y, m);

		// Z delta
		slope->zdelta = FixedDiv(m, slope->normal.z);
		
		// Get angles
		slope->real_xydirection = R_PointToAngle2(0, 0, slope->d.x, slope->d.y)+ANGLE_180;
		slope->real_zangle = InvAngle(R_PointToAngle2(0, 0, FRACUNIT, slope->zdelta));

		if (slope->normal.x == 0 && slope->normal.y == 0) { // Set some defaults for a non-sloped "slope"
			slope->zangle = slope->xydirection = 0;
			slope->zdelta = slope->d.x = slope->d.y = 0;
		} else {
			slope->xydirection = slope->real_xydirection;
			slope->zangle = slope->real_zangle;
		}
	}
}

/// Recalculate dynamic slopes.
void T_DynamicSlopeLine (dynplanethink_t* th)
{
	pslope_t* slope = th->slope;
	line_t* srcline = th->sourceline;

	fixed_t zdelta;

	switch(th->type) {
	case DP_FRONTFLOOR:
		zdelta = srcline->backsector->floorheight - srcline->frontsector->floorheight;
		slope->o.z = srcline->frontsector->floorheight;
		break;

	case DP_FRONTCEIL:
		zdelta = srcline->backsector->ceilingheight - srcline->frontsector->ceilingheight;
		slope->o.z = srcline->frontsector->ceilingheight;
		break;

	case DP_BACKFLOOR:
		zdelta = srcline->frontsector->floorheight - srcline->backsector->floorheight;
		slope->o.z = srcline->backsector->floorheight;
		break;

	case DP_BACKCEIL:
		zdelta = srcline->frontsector->ceilingheight - srcline->backsector->ceilingheight;
		slope->o.z = srcline->backsector->ceilingheight;
		break;

	default:
		return;
	}

	if (slope->zdelta != FixedDiv(zdelta, th->extent)) {
		slope->zdelta = FixedDiv(zdelta, th->extent);
		slope->zangle = R_PointToAngle2(0, 0, th->extent, -zdelta);
		slope->real_zangle = slope->zangle;
		P_CalculateSlopeNormal(slope);
	}
}

/// Mapthing-defined
void T_DynamicSlopeVert (dynplanethink_t* th)
{
	pslope_t* slope = th->slope;

	size_t i;
	INT32 l;

	for (i = 0; i < 3; i++) {
		l = P_FindSpecialLineFromTag(799, th->tags[i], -1);
		if (l != -1) {
			th->vex[i].z = lines[l].frontsector->floorheight;
		}
		else
			th->vex[i].z = 0;
	}

	ReconfigureViaVertexes(slope, th->vex[0], th->vex[1], th->vex[2]);
}

static inline void P_AddDynSlopeThinker (pslope_t* slope, dynplanetype_t type, line_t* sourceline, fixed_t extent, const INT16 tags[3], const vector3_t vx[3])
{
	dynplanethink_t* th = Z_Calloc(sizeof (*th), PU_LEVSPEC, NULL);
	switch (type)
	{
	case DP_VERTEX:
		th->thinker.function.acp1 = (actionf_p1)T_DynamicSlopeVert;
		memcpy(th->tags, tags, sizeof(th->tags));
		memcpy(th->vex, vx, sizeof(th->vex));
		break;
	default:
		th->thinker.function.acp1 = (actionf_p1)T_DynamicSlopeLine;
		th->sourceline = sourceline;
		th->extent = extent;
	}

	th->slope = slope;
	th->type = type;

	P_QueueSlopeThinker(&th->thinker);

	// interpolation
	R_CreateInterpolator_DynSlope(&th->thinker, slope);
}


/// Create a new slope and add it to the slope list.
static inline pslope_t* Slope_Add (const UINT8 flags)
{
	pslope_t *ret = Z_Calloc(sizeof(pslope_t), PU_LEVEL, NULL);
	ret->flags = flags;

	ret->next = slopelist;
	slopelist = ret;

	slopecount++;
	ret->id = slopecount;

	return ret;
}

/// Alocates and fill the contents of a slope structure.
static pslope_t *MakeViaVectors(const vector3_t *o, const vector2_t *d,
                             const fixed_t zdelta, UINT8 flags)
{
	pslope_t *ret = Slope_Add(flags);

	FV3_Copy(&ret->o, o);
	FV2_Copy(&ret->d, d);

	ret->zdelta = zdelta;

	ret->flags = flags;

	return ret;
}

/// Get furthest perpendicular distance from all vertexes in a sector for a given line.
static fixed_t GetExtent(sector_t *sector, line_t *line)
{
	// ZDoom code reference: v3float_t = vertex_t
	fixed_t fardist = -FRACUNIT;
	size_t i;

	// Find furthest vertex from the reference line. It, along with the two ends
	// of the line, will define the plane.
	for(i = 0; i < sector->linecount; i++)
	{
		line_t *li = sector->lines[i];
		vertex_t tempv;
		fixed_t dist;

		// Don't compare to the slope line.
		if(li == line)
			continue;

		P_ClosestPointOnLine(li->v1->x, li->v1->y, line, &tempv);
		dist = R_PointToDist2(tempv.x, tempv.y, li->v1->x, li->v1->y);
		if(dist > fardist)
			fardist = dist;

		// Okay, maybe do it for v2 as well?
		P_ClosestPointOnLine(li->v2->x, li->v2->y, line, &tempv);
		dist = R_PointToDist2(tempv.x, tempv.y, li->v2->x, li->v2->y);
		if(dist > fardist)
			fardist = dist;
	}

	return fardist;
}

/// Creates one or more slopes based on the given line type and front/back sectors.
static void line_SpawnViaLine(const int linenum, const boolean spawnthinker)
{
	// With dynamic slopes, it's fine to just leave this function as normal,
	// because checking to see if a slope had changed will waste more memory than
	// if the slope was just updated when called
	line_t *line = lines + linenum;
	INT16 special = line->special;
	pslope_t *fslope = NULL, *cslope = NULL;
	vector3_t origin, point;
	vector2_t direction;
	fixed_t nx, ny, dz, extent;

	boolean frontfloor = (special == 700 || special == 702 || special == 703);
	boolean backfloor  = (special == 710 || special == 712 || special == 713);
	boolean frontceil  = (special == 701 || special == 702 || special == 713);
	boolean backceil   = (special == 711 || special == 712 || special == 703);

	UINT8 flags = 0; // Slope flags
	if (line->flags & ML_NOSONIC)
		flags |= SL_NOPHYSICS;
	if (!(line->flags & ML_NOTAILS))
		flags |= SL_NODYNAMIC;

	if(!frontfloor && !backfloor && !frontceil && !backceil)
	{
		CONS_Printf("P_SpawnSlope_Line called with non-slope line special.\n");
		return;
	}

	if(!line->frontsector || !line->backsector)
	{
		CONS_Debug(DBG_SETUP, "P_SpawnSlope_Line used on a line without two sides. (line number %i)\n", linenum);
		return;
	}

	{
		fixed_t len = R_PointToDist2(0, 0, line->dx, line->dy);
		nx = FixedDiv(line->dy, len);
		ny = -FixedDiv(line->dx, len);
	}

	// Set origin to line's center.
	origin.x = line->v1->x + (line->v2->x - line->v1->x)/2;
	origin.y = line->v1->y + (line->v2->y - line->v1->y)/2;

	// For FOF slopes, make a special function to copy to the xy origin & direction relative to the position of the FOF on the map!
	if(frontfloor || frontceil)
	{
		line->frontsector->hasslope = true; // Tell the software renderer that we're sloped

		origin.z = line->backsector->floorheight;
		direction.x = nx;
		direction.y = ny;

		extent = GetExtent(line->frontsector, line);

		if(extent < 0)
		{
			CONS_Printf("P_SpawnSlope_Line failed to get frontsector extent on line number %i\n", linenum);
			return;
		}

		// reposition the origin according to the extent
		point.x = origin.x + FixedMul(direction.x, extent);
		point.y = origin.y + FixedMul(direction.y, extent);
		direction.x = -direction.x;
		direction.y = -direction.y;

		// TODO: We take origin and point 's xy values and translate them to the center of an FOF!

		if(frontfloor)
		{
			point.z = line->frontsector->floorheight; // Startz
			dz = FixedDiv(origin.z - point.z, extent); // Destinationz

			// In P_SpawnSlopeLine the origin is the centerpoint of the sourcelinedef

			fslope = line->frontsector->f_slope =
            MakeViaVectors(&point, &direction, dz, flags);

			fslope->zangle = R_PointToAngle2(0, origin.z, extent, point.z);
			fslope->xydirection = R_PointToAngle2(origin.x, origin.y, point.x, point.y);

			fslope->real_zangle = fslope->zangle;
			fslope->real_xydirection = fslope->xydirection;

			P_CalculateSlopeNormal(fslope);

			if (spawnthinker && !(flags & SL_NODYNAMIC))
				P_AddDynSlopeThinker(fslope, DP_FRONTFLOOR, line, extent, NULL, NULL);
		}
		if(frontceil)
		{
			origin.z = line->backsector->ceilingheight;
			point.z = line->frontsector->ceilingheight;
			dz = FixedDiv(origin.z - point.z, extent);

			cslope = line->frontsector->c_slope =
            MakeViaVectors(&point, &direction, dz, flags);

			cslope->zangle = R_PointToAngle2(0, origin.z, extent, point.z);
			cslope->xydirection = R_PointToAngle2(origin.x, origin.y, point.x, point.y);

			cslope->real_zangle = cslope->zangle;
			cslope->real_xydirection = cslope->xydirection;

			P_CalculateSlopeNormal(cslope);

			if (spawnthinker && !(flags & SL_NODYNAMIC))
				P_AddDynSlopeThinker(cslope, DP_FRONTCEIL, line, extent, NULL, NULL);
		}
	}
	if(backfloor || backceil)
	{
		line->backsector->hasslope = true; // Tell the software renderer that we're sloped

		origin.z = line->frontsector->floorheight;
		// Backsector
		direction.x = -nx;
		direction.y = -ny;

		extent = GetExtent(line->backsector, line);

		if(extent < 0)
		{
			CONS_Printf("P_SpawnSlope_Line failed to get backsector extent on line number %i\n", linenum);
			return;
		}

		// reposition the origin according to the extent
		point.x = origin.x + FixedMul(direction.x, extent);
		point.y = origin.y + FixedMul(direction.y, extent);
		direction.x = -direction.x;
		direction.y = -direction.y;

		if(backfloor)
		{
			point.z = line->backsector->floorheight;
			dz = FixedDiv(origin.z - point.z, extent);

			fslope = line->backsector->f_slope =
            MakeViaVectors(&point, &direction, dz, flags);

			fslope->zangle = R_PointToAngle2(0, origin.z, extent, point.z);
			fslope->xydirection = R_PointToAngle2(origin.x, origin.y, point.x, point.y);

			fslope->real_zangle = fslope->zangle;
			fslope->real_xydirection = fslope->xydirection;

			P_CalculateSlopeNormal(fslope);

			if (spawnthinker && !(flags & SL_NODYNAMIC))
				P_AddDynSlopeThinker(fslope, DP_BACKFLOOR, line, extent, NULL, NULL);
		}
		if(backceil)
		{
			origin.z = line->frontsector->ceilingheight;
			point.z = line->backsector->ceilingheight;
			dz = FixedDiv(origin.z - point.z, extent);

			cslope = line->backsector->c_slope =
            MakeViaVectors(&point, &direction, dz, flags);

			cslope->zangle = R_PointToAngle2(0, origin.z, extent, point.z);
			cslope->xydirection = R_PointToAngle2(origin.x, origin.y, point.x, point.y);

			cslope->real_zangle = cslope->zangle;
			cslope->real_xydirection = cslope->xydirection;

			P_CalculateSlopeNormal(cslope);

			if (spawnthinker && !(flags & SL_NODYNAMIC))
				P_AddDynSlopeThinker(cslope, DP_BACKCEIL, line, extent, NULL, NULL);
		}
	}

	if(!line->tag)
		return;
}

/// Creates a new slope from three mapthings with the specified IDs
static pslope_t *MakeViaMapthings(INT16 tag1, INT16 tag2, INT16 tag3, UINT8 flags, const boolean spawnthinker)
{
	size_t i;
	mapthing_t* mt = mapthings;
	mapthing_t* vertices[3] = {0};
	INT16 tags[3] = {tag1, tag2, tag3};

	vector3_t vx[3];
	pslope_t* ret = Slope_Add(flags);

	// And... look for the vertices in question.
	for (i = 0; i < nummapthings; i++, mt++) {
		if (mt->type != 750) // Haha, I'm hijacking the old Chaos Spawn thingtype for something!
			continue;

		if (!vertices[0] && mt->angle == tag1)
			vertices[0] = mt;
		else if (!vertices[1] && mt->angle == tag2)
			vertices[1] = mt;
		else if (!vertices[2] && mt->angle == tag3)
			vertices[2] = mt;
	}

	// Now set heights for each vertex, because they haven't been set yet
	for (i = 0; i < 3; i++) {
		mt = vertices[i];
		if (!mt) // If a vertex wasn't found, it's game over. There's nothing you can do to recover (except maybe try and kill the slope instead - TODO?)
			I_Error("MakeViaMapthings: Slope vertex %s (for linedef tag %d) not found!", sizeu1(i), tag1);
		vx[i].x = mt->x << FRACBITS;
		vx[i].y = mt->y << FRACBITS;
		if (mt->extrainfo)
			vx[i].z = mt->options << FRACBITS;
		else
			vx[i].z = (R_PointInSubsector(mt->x << FRACBITS, mt->y << FRACBITS)->sector->floorheight) + ((mt->options >> ZSHIFT) << FRACBITS);
	}

	ReconfigureViaVertexes(ret, vx[0], vx[1], vx[2]);

	if (spawnthinker && !(flags & SL_NODYNAMIC))
		P_AddDynSlopeThinker(ret, DP_VERTEX, NULL, 0, tags, vx);

	return ret;
}

/// Create vertex based slopes.
static void line_SpawnViaVertexes(const int linenum, const boolean spawnthinker)
{
	line_t *line = lines + linenum;
	side_t *side;
	pslope_t **slopetoset;
	UINT16 tag1, tag2, tag3;

	UINT8 flags = 0;
	if (line->flags & ML_NOSONIC)
		flags |= SL_NOPHYSICS;
	if (!(line->flags & ML_NOTAILS))
		flags |= SL_NODYNAMIC;

	switch(line->special)
	{
	case 704:
		slopetoset = &line->frontsector->f_slope;
		side = &sides[line->sidenum[0]];
		break;
	case 705:
		slopetoset = &line->frontsector->c_slope;
		side = &sides[line->sidenum[0]];
		break;
	case 714:
		slopetoset = &line->backsector->f_slope;
		side = &sides[line->sidenum[1]];
		break;
	case 715:
		slopetoset = &line->backsector->c_slope;
		side = &sides[line->sidenum[1]];
	default:
		return;
	}

	if (line->flags & ML_NOKNUX)
	{
		tag1 = line->tag;
		tag2 = side->textureoffset >> FRACBITS;
		tag3 = side->rowoffset >> FRACBITS;
	}
	else
		tag1 = tag2 = tag3 = line->tag;

	*slopetoset = MakeViaMapthings(tag1, tag2, tag3, flags, spawnthinker);

	side->sector->hasslope = true;
}


//
// P_CopySectorSlope
//
// Searches through tagged sectors and copies
//
void P_CopySectorSlope(line_t *line)
{
	sector_t *fsec = line->frontsector;
	int i, special = line->special;

	// Check for copy linedefs
	for(i = -1; (i = P_FindSectorFromLineTag(line, i)) >= 0;)
	{
		sector_t *srcsec = sectors + i;

		if ((special - 719) & 1 && !fsec->f_slope && srcsec->f_slope)
			fsec->f_slope = srcsec->f_slope; //P_CopySlope(srcsec->f_slope);
		if ((special - 719) & 2 && !fsec->c_slope && srcsec->c_slope)
			fsec->c_slope = srcsec->c_slope; //P_CopySlope(srcsec->c_slope);
	}

	fsec->hasslope = true;

	// if this is an FOF control sector, make sure any target sectors also are marked as having slopes
	if (fsec->numattached)
		for (i = 0; i < (int)fsec->numattached; i++)
			sectors[fsec->attached[i]].hasslope = true;

	line->special = 0; // Linedef was use to set slopes, it finished its job, so now make it a normal linedef
}

//
// P_SlopeById
//
// Looks in the slope list for a slope with a specified ID. Mostly useful for netgame sync
//
pslope_t *P_SlopeById(UINT16 id)
{
	pslope_t *ret;
	for (ret = slopelist; ret && ret->id != id; ret = ret->next);
	return ret;
}

/// Reset slopes and read them from special lines.
void P_ResetDynamicSlopes(const boolean fromsave)
{
	size_t i;

	/// Generates line special-defined slopes.
	for (i = 0; i < numlines; i++)
	{
		switch (lines[i].special)
		{
			case 700:
			case 701:
			case 702:
			case 703:
			case 710:
			case 711:
			case 712:
			case 713:
				line_SpawnViaLine(i, !fromsave);
				break;

			case 704:
			case 705:
			case 714:
			case 715:
				line_SpawnViaVertexes(i, !fromsave);
				break;

			default:
				break;
		}
	}
}

/// Initializes slopes.
void P_InitSlopes(void)
{
	slopelist = NULL;
	slopecount = 0;

	dynthinklist = NULL;
	dynthinknum = 0;
}


// ============================================================================
//
// Various utilities related to slopes
//

//
// P_GetZAt
//
// Returns the height of the sloped plane at (x, y) as a fixed_t
//
fixed_t P_GetZAt(pslope_t *slope, fixed_t x, fixed_t y)
{
   fixed_t dist = FixedMul(x - slope->o.x, slope->d.x) +
                  FixedMul(y - slope->o.y, slope->d.y);

   return slope->o.z + FixedMul(dist, slope->zdelta);
}

// Returns the height of the sector floor at (x, y)
fixed_t P_GetSectorFloorZAt(const sector_t *sector, fixed_t x, fixed_t y)
{
	return sector->f_slope ? P_GetZAt(sector->f_slope, x, y) : sector->floorheight;
}

// Returns the height of the sector ceiling at (x, y)
fixed_t P_GetSectorCeilingZAt(const sector_t *sector, fixed_t x, fixed_t y)
{
	return sector->c_slope ? P_GetZAt(sector->c_slope, x, y) : sector->ceilingheight;
}

// Returns the height of the FOF top at (x, y)
fixed_t P_GetFFloorTopZAt(const ffloor_t *ffloor, fixed_t x, fixed_t y)
{
	return *ffloor->t_slope ? P_GetZAt(*ffloor->t_slope, x, y) : *ffloor->topheight;
}

// Returns the height of the FOF bottom  at (x, y)
fixed_t P_GetFFloorBottomZAt(const ffloor_t *ffloor, fixed_t x, fixed_t y)
{
	return *ffloor->b_slope ? P_GetZAt(*ffloor->b_slope, x, y) : *ffloor->bottomheight;
}

// Returns the height of the light list at (x, y)
fixed_t P_GetLightZAt(const lightlist_t *light, fixed_t x, fixed_t y)
{
	return light->slope ? P_GetZAt(light->slope, x, y) : light->height;
}

//
// P_QuantizeMomentumToSlope
//
// When given a vector, rotates it and aligns it to a slope
void P_QuantizeMomentumToSlope(vector3_t *momentum, pslope_t *slope)
{
	vector3_t axis; // Fuck you, C90.

	if (slope->flags & SL_NOPHYSICS)
		return; // No physics, no quantizing.

	axis.x = -slope->d.y;
	axis.y = slope->d.x;
	axis.z = 0;

	FV3_Rotate(momentum, &axis, slope->zangle >> ANGLETOFINESHIFT);
}

//
// P_ReverseQuantizeMomentumToSlope
//
// When given a vector, rotates and aligns it to a flat surface (from being relative to a given slope)
void P_ReverseQuantizeMomentumToSlope(vector3_t *momentum, pslope_t *slope)
{
	slope->zangle = InvAngle(slope->zangle);
	P_QuantizeMomentumToSlope(momentum, slope);
	slope->zangle = InvAngle(slope->zangle);
}

// SRB2Kart: This fixes all slope-based jumps for different scales in Kart automatically without map tweaking.
// However, they will always feel off every single time... see for yourself: https://cdn.discordapp.com/attachments/270211093761097728/484924392128774165/kart0181.gif
//#define GROWNEVERMISSES

//
// P_SlopeLaunch
//
// Handles slope ejection for objects
void P_SlopeLaunch(mobj_t *mo)
{
	if (!(mo->standingslope->flags & SL_NOPHYSICS)) // If there's physics, time for launching.
	{
		// Double the pre-rotation Z, then halve the post-rotation Z. This reduces the
		// vertical launch given from slopes while increasing the horizontal launch
		// given. Good for SRB2's gravity and horizontal speeds.
		vector3_t slopemom;
		slopemom.x = mo->momx;
		slopemom.y = mo->momy;
		slopemom.z = mo->momz;
		P_QuantizeMomentumToSlope(&slopemom, mo->standingslope);

#ifdef GROWNEVERMISSES
		{
			const fixed_t xyscale = mapobjectscale + (mapobjectscale - mo->scale);
			const fixed_t zscale = mapobjectscale + (mapobjectscale - mo->scale);
			mo->momx = FixedMul(slopemom.x, xyscale);
			mo->momy = FixedMul(slopemom.y, xyscale);
			mo->momz = FixedMul(slopemom.z, zscale);
		}
#else
		mo->momx = slopemom.x;
		mo->momy = slopemom.y;
		mo->momz = slopemom.z;
#endif
	}

	//CONS_Printf("Launched off of slope.\n");
	mo->standingslope = NULL;
}

// Function to help handle landing on slopes
void P_HandleSlopeLanding(mobj_t *thing, pslope_t *slope)
{
	vector3_t mom; // Ditto.

	if (slope->flags & SL_NOPHYSICS) { // No physics, no need to make anything complicated.
		if (P_MobjFlip(thing)*(thing->momz) < 0) { // falling, land on slope
			thing->momz = -P_MobjFlip(thing);
			thing->standingslope = slope;
		}
		return;
	}

	mom.x = thing->momx;
	mom.y = thing->momy;
	mom.z = thing->momz*2;

	P_ReverseQuantizeMomentumToSlope(&mom, slope);

	if (P_MobjFlip(thing)*mom.z < 0) { // falling, land on slope
		thing->momx = mom.x;
		thing->momy = mom.y;
		thing->momz = -P_MobjFlip(thing);

		thing->standingslope = slope;
	}
}

// https://yourlogicalfallacyis.com/slippery-slope
// Handles sliding down slopes, like if they were made of butter :)
void P_ButteredSlope(mobj_t *mo)
{
	fixed_t thrust;

	if (!mo->standingslope)
		return;

	if (mo->standingslope->flags & SL_NOPHYSICS)
		return; // No physics, no butter.

	if (mo->flags & (MF_NOCLIPHEIGHT|MF_NOGRAVITY))
		return; // don't slide down slopes if you can't touch them or you're not affected by gravity

	if (mo->player) {
		if (abs(mo->standingslope->zdelta) < FRACUNIT/4 && !(mo->player->pflags & PF_SPINNING))
			return; // Don't slide on non-steep slopes unless spinning

		if (abs(mo->standingslope->zdelta) < FRACUNIT/2 && !(mo->player->rmomx || mo->player->rmomy))
			return; // Allow the player to stand still on slopes below a certain steepness
	}

	thrust = FINESINE(mo->standingslope->zangle>>ANGLETOFINESHIFT) * 15 / 16 * (mo->eflags & MFE_VERTICALFLIP ? 1 : -1);

	if (mo->player && (mo->player->pflags & PF_SPINNING)) {
		fixed_t mult = 0;
		if (mo->momx || mo->momy) {
			angle_t angle = R_PointToAngle2(0, 0, mo->momx, mo->momy) - mo->standingslope->xydirection;

			if (P_MobjFlip(mo) * mo->standingslope->zdelta < 0)
				angle ^= ANGLE_180;

			mult = FINECOSINE(angle >> ANGLETOFINESHIFT);
		}

		thrust = FixedMul(thrust, FRACUNIT*2/3 + mult/8);
	}

	if (mo->momx || mo->momy) // Slightly increase thrust based on the object's speed
		thrust = FixedMul(thrust, FRACUNIT+P_AproxDistance(mo->momx, mo->momy)/16);
	// This makes it harder to zigzag up steep slopes, as well as allows greater top speed when rolling down

	// Let's get the gravity strength for the object...
	thrust = FixedMul(thrust, abs(P_GetMobjGravity(mo)));

	// ... and its friction against the ground for good measure (divided by original friction to keep behaviour for normal slopes the same).
	thrust = FixedMul(thrust, FixedDiv(mo->friction, ORIG_FRICTION));

	P_Thrust(mo, mo->standingslope->xydirection, thrust);
}

