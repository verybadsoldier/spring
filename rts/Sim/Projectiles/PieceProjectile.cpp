/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Sim/Projectiles/PieceProjectile.h"
#include "Game/Camera.h"
#include "Game/GameHelper.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/Textures/TextureAtlas.h"
#include "Rendering/Colors.h"
#include "Rendering/Env/Particles/ProjectileDrawer.h"
#include "Rendering/Env/Particles/Classes/SmokeTrailProjectile.h"
#include "Rendering/Models/3DModel.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Projectiles/ExplosionGenerator.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/ProjectileMemPool.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "System/Matrix44f.h"
#include "System/SpringMath.h"

static constexpr int   SMOKE_TIME   = 40;
static constexpr int   SMOKE_SIZE   = 14;
static constexpr float SMOKE_COLOR  = 0.5f;

CR_BIND_DERIVED(CPieceProjectile, CProjectile, )
CR_REG_METADATA(CPieceProjectile,(
	CR_SETFLAG(CF_Synced),

	CR_MEMBER(age),
	CR_MEMBER(explFlags),
	CR_IGNORED(omp),
	CR_IGNORED(smokeTrail),
	CR_IGNORED(fireTrailPoints),
	CR_MEMBER(spinVec),
	CR_MEMBER(spinSpeed),
	CR_MEMBER(spinAngle),
	CR_MEMBER(oldSmokePos),
	CR_MEMBER(oldSmokeDir)
))

CPieceProjectile::CPieceProjectile(
	CUnit* owner,
	LocalModelPiece* lmp,
	const float3& pos,
	const float3& speed,
	int flags,
	float radius
):
	CProjectile(pos, speed, owner, true, false, true),
	age(0),
	explFlags(flags),
	omp((lmp != nullptr) ? lmp->original : nullptr),
	smokeTrail(nullptr)
{
	if (owner != nullptr) {
		const UnitDef* ud = owner->unitDef;

		if ((explFlags & PF_NoCEGTrail) == 0 && ud->GetPieceExpGenCount() > 0) {
			cegID = guRNG.NextInt(ud->GetPieceExpGenCount());
			cegID = ud->GetPieceExpGenID(cegID);

			explGenHandler.GenExplosion(cegID, pos, speed, 100, 0.0f, 0.0f, nullptr, nullptr);
		}

		model = owner->model;
		explFlags |= (PF_NoCEGTrail * (cegID == -1u));
	}
	{
		// neither spinVector nor spinParams technically need to be
		// synced, but since instances of this class are themselves
		// synced and have LuaSynced{Ctrl, Read} exposure we treat
		// them that way for consistency
		spinVec = gsRNG.NextVector();
		spinSpeed = gsRNG.NextFloat() * 20.0f;
		spinAngle = 0.0f;

		oldSmokePos = pos;
		oldSmokeDir = speed;

		spinVec.Normalize();
		oldSmokeDir.Normalize();
	}

	for (auto& ftp: fireTrailPoints) {
		ftp = {pos, 0.0f};
	}

	SetRadiusAndHeight(radius, 0.0f);

	checkCol = false;
	castShadow = true;
	useAirLos = (pos.y - CGround::GetApproximateHeight(pos.x, pos.z) > 10.0f);

	projectileHandler.AddProjectile(this);
	assert(!detached);
}


void CPieceProjectile::Collision()
{
	Collision(nullptr, nullptr);
	if (gsRNG.NextFloat() < 0.666f) { // give it a small chance to `ground bounce`
		CProjectile::Collision();
		return;
	}

	// ground bounce
	const float3& norm = CGround::GetNormal(pos.x, pos.z);
	const float ns = speed.dot(norm);
	SetVelocityAndSpeed(speed - (norm * ns * 1.6f));
	SetPosition(pos + (norm * 0.1f));
}


void CPieceProjectile::Collision(CFeature* f)
{
	Collision(nullptr, f);
	CProjectile::Collision(f);
}


void CPieceProjectile::Collision(CUnit* unit)
{
	Collision(unit, nullptr);
	CProjectile::Collision(unit);
}


void CPieceProjectile::Collision(CUnit* unit, CFeature* feature)
{
	if (unit && (unit == owner()))
		return;

	if ((explFlags & PF_Explode) && (unit || feature)) {
		const DamageArray damageArray(modInfo.debrisDamage);
		const CExplosionParams params = {
			pos,
			ZeroVector,
			damageArray,
			nullptr,           // weaponDef
			owner(),
			unit,              // hitUnit
			feature,           // hitFeature
			0.0f,              // craterAreaOfEffect
			5.0f,              // damageAreaOfEffect
			0.0f,              // edgeEffectiveness
			10.0f,             // explosionSpeed
			1.0f,              // gfxMod
			true,              // impactOnly
			false,             // ignoreOwner
			false,             // damageGround
			static_cast<unsigned int>(id)
		};

		helper->Explosion(params);
	}

	if (explFlags & PF_Smoke) {
		if (explFlags & PF_NoCEGTrail) {
			smokeTrail = projMemPool.alloc<CSmokeTrailProjectile>(
				owner(),
				pos, oldSmokePos,
				dir, oldSmokeDir,
				false,
				true,
				SMOKE_SIZE,
				SMOKE_TIME,
				NUM_TRAIL_PARTS,
				SMOKE_COLOR,
				projectileDrawer->smoketrailtex
			);
		}
	}

	oldSmokePos = pos;
}



float3 CPieceProjectile::RandomVertexPos() const
{
	if (omp == nullptr)
		return ZeroVector;
	#define rf guRNG.NextFloat()
	return mix(omp->mins, omp->maxs, float3(rf,rf,rf));
}


float CPieceProjectile::GetDrawAngle() const
{
	return spinAngle + spinSpeed * globalRendering->timeOffset;
}


void CPieceProjectile::Update()
{
	if (!luaMoveCtrl) {
		speed.y += mygravity;
		SetVelocityAndSpeed(speed * 0.997f);
		SetPosition(pos + speed);
	}

	spinAngle += spinSpeed;
	age += 1;
	checkCol |= (age > 10);

	if ((explFlags & PF_NoCEGTrail) == 0) {
		// TODO: pass a more sensible ttl to the CEG (age-related?)
		explGenHandler.GenExplosion(cegID, pos, speed, 100, 0.0f, 0.0f, nullptr, nullptr);
		return;
	}

	if (explFlags & PF_Fire) {
		for (int a = NUM_TRAIL_PARTS - 2; a >= 0; --a) {
			fireTrailPoints[a + 1] = fireTrailPoints[a];
		}

		CMatrix44f m(pos);
		m.Rotate(spinAngle * math::DEG_TO_RAD, spinVec);
		m.Translate(RandomVertexPos());

		fireTrailPoints[0].pos  = m.GetPos();
		fireTrailPoints[0].size = 1 + guRNG.NextFloat();
	}

	if (explFlags & PF_Smoke) {
		if (smokeTrail) {
			smokeTrail->UpdateEndPos(pos, dir);
			oldSmokePos = pos;
			oldSmokeDir = dir;
		}

		if ((age % NUM_TRAIL_PARTS) == 0) {
			smokeTrail = projMemPool.alloc<CSmokeTrailProjectile>(
				owner(),
				pos, oldSmokePos,
				dir, oldSmokeDir,
				age == (NUM_TRAIL_PARTS - 1),
				false,
				SMOKE_SIZE,
				SMOKE_TIME,
				NUM_TRAIL_PARTS,
				SMOKE_COLOR,
				projectileDrawer->smoketrailtex
			);

			useAirLos = smokeTrail->useAirLos;
		}
	}
}


void CPieceProjectile::DrawOnMinimap()
{
	AddMiniMapVertices({ pos        , color4::red }, { pos + speed, color4::red });
}


void CPieceProjectile::Draw()
{
	if ((explFlags & PF_Fire) == 0)
		return;

	static const SColor lightOrange(1.f, 0.78f, 0.59f, 0.2f);

	for (unsigned int age = 0; age < NUM_TRAIL_PARTS; ++age) {
		const float3 interPos = fireTrailPoints[age].pos;
		const float size = fireTrailPoints[age].size;

		const float alpha = 1.0f - (age * (1.0f / NUM_TRAIL_PARTS));
		const float drawsize = (1.0f + age) * size;
		const SColor col = lightOrange * alpha;

		const auto eft = projectileDrawer->explofadetex;
		AddEffectsQuad(
			{ interPos - camera->GetRight() * drawsize - camera->GetUp() * drawsize, eft->xstart, eft->ystart, col },
			{ interPos + camera->GetRight() * drawsize - camera->GetUp() * drawsize, eft->xend,   eft->ystart, col },
			{ interPos + camera->GetRight() * drawsize + camera->GetUp() * drawsize, eft->xend,   eft->yend,   col },
			{ interPos - camera->GetRight() * drawsize + camera->GetUp() * drawsize, eft->xstart, eft->yend,   col }
		);
	}
}


int CPieceProjectile::GetProjectilesCount() const
{
	return NUM_TRAIL_PARTS;
}
