/*
* Copyright (c) 2013 Google, Inc.
*
* This software is provided 'as-is', without any express or implied
* warranty.  In no event will the authors be held liable for any damages
* arising from the use of this software.
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 3. This notice may not be removed or altered from any source distribution.
*/
#include <Box2D/Particle/b2ParticleSystem.h>
#include <Box2D/Particle/b2ParticleGroup.h>
#include <Box2D/Particle/b2VoronoiDiagram.h>
#include <Box2D/Common/b2BlockAllocator.h>
#include <Box2D/Dynamics/b2World.h>
#include <Box2D/Dynamics/b2WorldCallbacks.h>
#include <Box2D/Dynamics/b2Body.h>
#include <Box2D/Dynamics/b2Fixture.h>
#include <Box2D/Collision/Shapes/b2Shape.h>
#include <Box2D/Collision/Shapes/b2EdgeShape.h>
#include <Box2D/Collision/Shapes/b2ChainShape.h>
#include <algorithm>

static const uint32 xTruncBits = 12;
static const uint32 yTruncBits = 12;
static const uint32 tagBits = 8 * sizeof(uint32);
static const uint32 yOffset = 1 << (yTruncBits - 1);
static const uint32 yShift = tagBits - yTruncBits;
static const uint32 xShift = tagBits - yTruncBits - xTruncBits;
static const uint32 xScale = 1 << xShift;
static const uint32 xOffset = xScale * (1 << (xTruncBits - 1));
static const uint32 xMask = (1 << xTruncBits) - 1;
static const uint32 yMask = (1 << yTruncBits) - 1;


// This functor is passed to std::remove_if in RemoveSpuriousBodyContacts
// to implement the algorithm described there.  It was hoisted out and friended
// as it would not compile with g++ 4.6.3 as a local class.  It is only used in
// that function.
class b2ParticleBodyContactRemovePredicate
{
public:
	b2ParticleBodyContactRemovePredicate(b2ParticleSystem* system,
										 int32* discarded)
		: m_system(system), m_lastIndex(-1), m_currentContacts(0),
		  m_discarded(discarded) {}

	bool operator()(const b2ParticleBodyContact& contact)
	{
		// This implements the selection criteria described in
		// RemoveSpuriousBodyContacts().
		// This functor is iterating through a list of Body contacts per
		// Particle, ordered from near to far.  For up to the maximum number of
		// contacts we allow per point per step, we verify that the contact
		// normal of the Body that genenerated the contact makes physical sense
		// by projecting a point back along that normal and seeing if it
		// intersects the fixture generating the contact.

		if (contact.index != m_lastIndex)
		{
			m_currentContacts = 0;
			m_lastIndex = contact.index;
		}

		if (m_currentContacts++ > k_maxContactsPerPoint)
		{
			++(*m_discarded);
			return true;
		}

		// Project along inverse normal (as returned in the contact) to get the
		// point to check.
		b2Vec2 n = contact.normal;
		// weight is 1-(inv(diameter) * distance)
		n *= m_system->m_particleDiameter * (1 - contact.weight);
		b2Vec2 pos = m_system->m_positionBuffer.data[contact.index] + n;

		// pos is now a point projected back along the contact normal to the
		// contact distance. If the surface makes sense for a contact, pos will
		// now lie on or in the fixture generating
		if (!contact.fixture->TestPoint(pos))
		{
			++(*m_discarded);
			return true;
		}

		return false;
	}
private:
	// Max number of contacts processed per particle, from nearest to farthest.
	// This must be at least 2 for correctness with concave shapes; 3 was
	// experimentally arrived at as looking reasonable.
	static const int32 k_maxContactsPerPoint = 3;
	const b2ParticleSystem* m_system;
	// Index of last particle processed.
	int32 m_lastIndex;
	// Number of contacts processed for the current particle.
	int32 m_currentContacts;
	// Output the number of discarded contacts.
	int32* m_discarded;
};

static inline uint32 computeTag(float32 x, float32 y)
{
	return ((uint32)(y + yOffset) << yShift) + (uint32)(xScale * x + xOffset);
}

static inline uint32 computeRelativeTag(uint32 tag, int32 x, int32 y)
{
	return tag + (y << yShift) + (x << xShift);
}

b2ParticleSystem::b2ParticleSystem(const b2ParticleSystemDef* def, b2World* world)
{

	m_timestamp = 0;
	m_allParticleFlags = 0;
	m_needsUpdateAllParticleFlags = false;
	m_allGroupFlags = 0;
	m_needsUpdateAllGroupFlags = false;
	m_iterationIndex = 0;
	m_strictContactCheck = false;

	m_density = 1;
	m_inverseDensity = 1;
	m_gravityScale = 1;
	SetParticleRadius(def->particleRadius);

	m_count = 0;
	m_internalAllocatedCapacity = 0;
	m_maxCount = 0;
	m_weightBuffer = NULL;
	m_staticPressureBuffer = NULL;
	m_accumulationBuffer = NULL;
	m_accumulation2Buffer = NULL;
	m_depthBuffer = NULL;
	m_groupBuffer = NULL;

	m_proxyCount = 0;
	m_proxyCapacity = 0;
	m_proxyBuffer = NULL;

	m_contactCount = 0;
	m_contactCapacity = 0;
	m_contactBuffer = NULL;

	m_bodyContactCount = 0;
	m_bodyContactCapacity = 0;
	m_bodyContactBuffer = NULL;

	m_pairCount = 0;
	m_pairCapacity = 0;
	m_pairBuffer = NULL;

	m_triadCount = 0;
	m_triadCapacity = 0;
	m_triadBuffer = NULL;

	m_groupCount = 0;
	m_groupList = NULL;

	m_def = *def;

	m_world = world;
}

b2ParticleSystem::~b2ParticleSystem()
{
	while (m_groupList)
	{
		DestroyParticleGroup(m_groupList);
	}

	FreeParticleBuffer(&m_flagsBuffer);
	FreeParticleBuffer(&m_positionBuffer);
	FreeParticleBuffer(&m_velocityBuffer);
	FreeParticleBuffer(&m_colorBuffer);
	FreeParticleBuffer(&m_userDataBuffer);
	FreeBuffer(&m_weightBuffer, m_internalAllocatedCapacity);
	FreeBuffer(&m_staticPressureBuffer, m_internalAllocatedCapacity);
	FreeBuffer(&m_accumulationBuffer, m_internalAllocatedCapacity);
	FreeBuffer(&m_accumulation2Buffer, m_internalAllocatedCapacity);
	FreeBuffer(&m_depthBuffer, m_internalAllocatedCapacity);
	FreeBuffer(&m_groupBuffer, m_internalAllocatedCapacity);
	FreeBuffer(&m_proxyBuffer, m_proxyCapacity);
	FreeBuffer(&m_contactBuffer, m_contactCapacity);
	FreeBuffer(&m_bodyContactBuffer, m_bodyContactCapacity);
	FreeBuffer(&m_pairBuffer, m_pairCapacity);
	FreeBuffer(&m_triadBuffer, m_triadCapacity);
}

template <typename T> void b2ParticleSystem::FreeBuffer(T** b, int capacity)
{
	if (*b == NULL)
		return;

	m_world->m_blockAllocator.Free(*b, sizeof(**b) * capacity);
	*b = NULL;
}

// Free buffer, if it was allocated with b2World's block allocator
template <typename T> void b2ParticleSystem::FreeParticleBuffer(
	ParticleBuffer<T>* b)
{
	if (b->userSuppliedCapacity == 0)
	{
		FreeBuffer(&b->data, m_internalAllocatedCapacity);
	}
}

// Reallocate a buffer
template <typename T> T* b2ParticleSystem::ReallocateBuffer(
	T* oldBuffer, int32 oldCapacity, int32 newCapacity)
{
	b2Assert(newCapacity > oldCapacity);
	T* newBuffer = (T*) m_world->m_blockAllocator.Allocate(
		sizeof(T) * newCapacity);
	memcpy(newBuffer, oldBuffer, sizeof(T) * oldCapacity);
	m_world->m_blockAllocator.Free(oldBuffer, sizeof(T) * oldCapacity);
	return newBuffer;
}

// Reallocate a buffer
template <typename T> T* b2ParticleSystem::ReallocateBuffer(
	T* buffer, int32 userSuppliedCapacity, int32 oldCapacity,
	int32 newCapacity, bool deferred)
{
	b2Assert(newCapacity > oldCapacity);
	// A 'deferred' buffer is reallocated only if it is not NULL.
	// If 'userSuppliedCapacity' is not zero, buffer is user supplied and must
	// be kept.
	b2Assert(!userSuppliedCapacity || newCapacity <= userSuppliedCapacity);
	if ((!deferred || buffer) && !userSuppliedCapacity)
	{
		buffer = ReallocateBuffer(buffer, oldCapacity, newCapacity);
	}
	return buffer;
}

// Reallocate a buffer
template <typename T> T* b2ParticleSystem::ReallocateBuffer(
	ParticleBuffer<T>* buffer, int32 oldCapacity, int32 newCapacity,
	bool deferred)
{
	b2Assert(newCapacity > oldCapacity);
	return ReallocateBuffer(buffer->data, buffer->userSuppliedCapacity,
							oldCapacity, newCapacity, deferred);
}

template <typename T> T* b2ParticleSystem::RequestParticleBuffer(T* buffer)
{
	if (!buffer)
	{
		if (m_internalAllocatedCapacity == 0)
		{
			ReallocateInternalAllocatedBuffers(b2_minParticleBufferCapacity);
		}
		buffer = (T*) (m_world->m_blockAllocator.Allocate(
						   sizeof(T) * m_internalAllocatedCapacity));
		b2Assert(buffer);
		memset(buffer, 0, sizeof(T) * m_internalAllocatedCapacity);
	}
	return buffer;
}

static int32 LimitCapacity(int32 capacity, int32 maxCount)
{
	return maxCount && capacity > maxCount ? maxCount : capacity;
}

void b2ParticleSystem::ReallocateInternalAllocatedBuffers(int32 capacity)
{
	// Don't increase capacity beyond the smallest user-supplied buffer size.
	capacity = LimitCapacity(capacity, m_maxCount);
	capacity = LimitCapacity(capacity, m_flagsBuffer.userSuppliedCapacity);
	capacity = LimitCapacity(capacity, m_positionBuffer.userSuppliedCapacity);
	capacity = LimitCapacity(capacity, m_velocityBuffer.userSuppliedCapacity);
	capacity = LimitCapacity(capacity, m_colorBuffer.userSuppliedCapacity);
	capacity = LimitCapacity(capacity, m_userDataBuffer.userSuppliedCapacity);
	if (m_internalAllocatedCapacity < capacity)
	{
		m_flagsBuffer.data = ReallocateBuffer(
			&m_flagsBuffer, m_internalAllocatedCapacity, capacity, false);
		m_positionBuffer.data = ReallocateBuffer(
			&m_positionBuffer, m_internalAllocatedCapacity, capacity, false);
		m_velocityBuffer.data = ReallocateBuffer(
			&m_velocityBuffer, m_internalAllocatedCapacity, capacity, false);
		m_weightBuffer = ReallocateBuffer(
			m_weightBuffer, 0, m_internalAllocatedCapacity, capacity, false);
		m_staticPressureBuffer = ReallocateBuffer(
			m_staticPressureBuffer, 0, m_internalAllocatedCapacity, capacity,
			true);
		m_accumulationBuffer = ReallocateBuffer(
			m_accumulationBuffer, 0, m_internalAllocatedCapacity, capacity,
			false);
		m_accumulation2Buffer = ReallocateBuffer(
			m_accumulation2Buffer, 0, m_internalAllocatedCapacity, capacity,
			true);
		m_depthBuffer = ReallocateBuffer(
			m_depthBuffer, 0, m_internalAllocatedCapacity, capacity, true);
		m_colorBuffer.data = ReallocateBuffer(
			&m_colorBuffer, m_internalAllocatedCapacity, capacity, true);
		m_groupBuffer = ReallocateBuffer(
			m_groupBuffer, 0, m_internalAllocatedCapacity, capacity, false);
		m_userDataBuffer.data = ReallocateBuffer(
			&m_userDataBuffer, m_internalAllocatedCapacity, capacity, true);
		m_internalAllocatedCapacity = capacity;
	}
}

int32 b2ParticleSystem::CreateParticle(const b2ParticleDef& def)
{
	b2Assert(m_world->IsLocked() == false);
	if (m_world->IsLocked())
	{
		return 0;
	}

	if (m_count >= m_internalAllocatedCapacity)
	{
		// Double the particle capacity.
		int32 capacity = m_count ? 2 * m_count : b2_minParticleBufferCapacity;
		ReallocateInternalAllocatedBuffers(capacity);
	}
	if (m_count >= m_internalAllocatedCapacity)
	{
		return b2_invalidParticleIndex;
	}
	int32 index = m_count++;
	m_flagsBuffer.data[index] = 0;
	m_positionBuffer.data[index] = def.position;
	m_velocityBuffer.data[index] = def.velocity;
	m_weightBuffer[index] = 0;
	if (m_staticPressureBuffer)
	{
		m_staticPressureBuffer[index] = 0;
	}
	m_groupBuffer[index] = NULL;
	if (m_depthBuffer)
	{
		m_depthBuffer[index] = 0;
	}
	if (m_colorBuffer.data || !def.color.IsZero())
	{
		m_colorBuffer.data = RequestParticleBuffer(m_colorBuffer.data);
		m_colorBuffer.data[index] = def.color;
	}
	if (m_userDataBuffer.data || def.userData)
	{
		m_userDataBuffer.data= RequestParticleBuffer(m_userDataBuffer.data);
		m_userDataBuffer.data[index] = def.userData;
	}
	if (m_proxyCount >= m_proxyCapacity)
	{
		int32 oldCapacity = m_proxyCapacity;
		int32 newCapacity = m_proxyCount ?
			2 * m_proxyCount : b2_minParticleBufferCapacity;
		m_proxyBuffer = ReallocateBuffer(m_proxyBuffer, oldCapacity,
										 newCapacity);
		m_proxyCapacity = newCapacity;
	}
	m_proxyBuffer[m_proxyCount++].index = index;
	SetParticleFlags(index, def.flags);
	return index;
}

void b2ParticleSystem::DestroyParticle(
	int32 index, bool callDestructionListener)
{
	uint32 flags = b2_zombieParticle;
	if (callDestructionListener)
	{
		flags |= b2_destructionListener;
	}
	SetParticleFlags(index, m_flagsBuffer.data[index] | flags);
}

int32 b2ParticleSystem::DestroyParticlesInShape(
	const b2Shape& shape, const b2Transform& xf,
	bool callDestructionListener)
{
	b2Assert(m_world->IsLocked() == false);
	if (m_world->IsLocked())
	{
		return 0;
	}

	class DestroyParticlesInShapeCallback : public b2QueryCallback
	{
	public:
		DestroyParticlesInShapeCallback(
			b2ParticleSystem* system, const b2Shape& shape,
			const b2Transform& xf, bool callDestructionListener)
		{
			m_system = system;
			m_shape = &shape;
			m_xf = xf;
			m_callDestructionListener = callDestructionListener;
			m_destroyed = 0;
		}

		bool ReportFixture(b2Fixture* fixture)
		{
			B2_NOT_USED(fixture);
			return false;
		}

		bool ReportParticle(int32 index)
		{
			b2Assert(index >=0 && index < m_system->m_count);
			if (m_shape->TestPoint(m_xf,
								   m_system->m_positionBuffer.data[index]))
			{
				m_system->DestroyParticle(index, m_callDestructionListener);
				m_destroyed++;
			}
			return true;
		}

		int32 Destroyed() { return m_destroyed; }

	private:
		b2ParticleSystem* m_system;
		const b2Shape* m_shape;
		b2Transform m_xf;
		bool m_callDestructionListener;
		int32 m_destroyed;
	} callback(this, shape, xf, callDestructionListener);
	b2AABB aabb;
	shape.ComputeAABB(&aabb, xf, 0);
	m_world->QueryAABB(&callback, aabb);
	return callback.Destroyed();
}

void b2ParticleSystem::DestroyParticlesInGroup(
	b2ParticleGroup* group, bool callDestructionListener)
{
	b2Assert(m_world->IsLocked() == false);
	if (m_world->IsLocked())
	{
		return;
	}

	for (int32 i = group->m_firstIndex; i < group->m_lastIndex; i++) {
		DestroyParticle(i, callDestructionListener);
	}
}

int32 b2ParticleSystem::CreateParticleForGroup(
	const b2ParticleGroupDef& groupDef, const b2Transform& xf, const b2Vec2& p)
{
	b2ParticleDef particleDef;
	particleDef.flags = groupDef.flags;
	particleDef.position = b2Mul(xf, p);
	particleDef.velocity =
		groupDef.linearVelocity +
		b2Cross(groupDef.angularVelocity,
				particleDef.position - groupDef.position);
	particleDef.color = groupDef.color;
	particleDef.userData = groupDef.userData;
	return CreateParticle(particleDef);
}

void b2ParticleSystem::CreateParticlesStrokeShapeForGroup(
	const b2ParticleGroupDef& groupDef, const b2Transform& xf)
{
	const b2Shape *shape = groupDef.shape;
	float32 stride = groupDef.stride;
	if (stride == 0)
	{
		stride = GetParticleStride();
	}
	float32 positionOnEdge = 0;
	int32 childCount = shape->GetChildCount();
	for (int32 childIndex = 0; childIndex < childCount; childIndex++)
	{
		b2EdgeShape edge;
		if (shape->GetType() == b2Shape::e_edge)
		{
			edge = *(b2EdgeShape*) shape;
		}
		else
		{
			b2Assert(shape->GetType() == b2Shape::e_chain);
			((b2ChainShape*) shape)->GetChildEdge(&edge, childIndex);
		}
		b2Vec2 d = edge.m_vertex2 - edge.m_vertex1;
		float32 edgeLength = d.Length();
		while (positionOnEdge < edgeLength)
		{
			b2Vec2 p = edge.m_vertex1 + positionOnEdge / edgeLength * d;
			CreateParticleForGroup(groupDef, xf, p);
			positionOnEdge += stride;
		}
		positionOnEdge -= edgeLength;
	}
}

void b2ParticleSystem::CreateParticlesFillShapeForGroup(
	const b2ParticleGroupDef& groupDef, const b2Transform& xf)
{
	const b2Shape *shape = groupDef.shape;
	float32 stride = groupDef.stride;
	if (stride == 0)
	{
		stride = GetParticleStride();
	}
	b2Transform identity;
	identity.SetIdentity();
	b2AABB aabb;
	b2Assert(shape->GetChildCount() == 1);
	shape->ComputeAABB(&aabb, identity, 0);
	for (float32 y = floorf(aabb.lowerBound.y / stride) * stride;
		y < aabb.upperBound.y; y += stride)
	{
		for (float32 x = floorf(aabb.lowerBound.x / stride) * stride;
			x < aabb.upperBound.x; x += stride)
		{
			b2Vec2 p(x, y);
			if (shape->TestPoint(identity, p))
			{
				CreateParticleForGroup(groupDef, xf, p);
			}
		}
	}
}

b2ParticleGroup* b2ParticleSystem::CreateParticleGroup(
	const b2ParticleGroupDef& groupDef)
{
	b2Assert(m_world->IsLocked() == false);
	if (m_world->IsLocked())
	{
		return 0;
	}

	b2Transform transform;
	transform.Set(groupDef.position, groupDef.angle);
	int32 firstIndex = m_count;
	if (groupDef.shape)
	{
		const b2Shape *shape = groupDef.shape;
		switch (shape->GetType()) {
		case b2Shape::e_edge:
		case b2Shape::e_chain:
			CreateParticlesStrokeShapeForGroup(groupDef, transform);
			break;
		case b2Shape::e_polygon:
		case b2Shape::e_circle:
			CreateParticlesFillShapeForGroup(groupDef, transform);
			break;
		default:
			b2Assert(false);
			break;
		}
	}
	if (groupDef.particleCount)
	{
		b2Assert(groupDef.positionData);
		for (int32 i = 0; i < groupDef.particleCount; i++)
		{
			b2Vec2 p = groupDef.positionData[i];
			CreateParticleForGroup(groupDef, transform, p);
		}
	}
	int32 lastIndex = m_count;

	void* mem = m_world->m_blockAllocator.Allocate(sizeof(b2ParticleGroup));
	b2ParticleGroup* group = new (mem) b2ParticleGroup();
	group->m_system = this;
	group->m_firstIndex = firstIndex;
	group->m_lastIndex = lastIndex;
	group->m_strength = groupDef.strength;
	group->m_userData = groupDef.userData;
	group->m_transform = transform;
	group->m_prev = NULL;
	group->m_next = m_groupList;
	if (m_groupList)
	{
		m_groupList->m_prev = group;
	}
	m_groupList = group;
	++m_groupCount;
	for (int32 i = firstIndex; i < lastIndex; i++)
	{
		m_groupBuffer[i] = group;
	}
	SetParticleGroupFlags(group, groupDef.groupFlags);

	UpdateContacts(true);
	UpdatePairsAndTriads(firstIndex, lastIndex, group, group);

	return group;
}

void b2ParticleSystem::JoinParticleGroups(b2ParticleGroup* groupA,
										  b2ParticleGroup* groupB)
{
	b2Assert(m_world->IsLocked() == false);
	if (m_world->IsLocked())
	{
		return;
	}

	b2Assert(groupA != groupB);
	RotateBuffer(groupB->m_firstIndex, groupB->m_lastIndex, m_count);
	b2Assert(groupB->m_lastIndex == m_count);
	RotateBuffer(groupA->m_firstIndex, groupA->m_lastIndex,
				 groupB->m_firstIndex);
	b2Assert(groupA->m_lastIndex == groupB->m_firstIndex);

	UpdateContacts(true);
	UpdatePairsAndTriads(
		groupA->m_firstIndex, groupB->m_lastIndex, groupA, groupB);

	for (int32 i = groupB->m_firstIndex; i < groupB->m_lastIndex; i++)
	{
		m_groupBuffer[i] = groupA;
	}
	uint32 groupFlags = groupA->m_groupFlags | groupB->m_groupFlags;
	SetParticleGroupFlags(groupA, groupFlags);
	groupA->m_lastIndex = groupB->m_lastIndex;
	groupB->m_firstIndex = groupB->m_lastIndex;
	DestroyParticleGroup(groupB);
}

void b2ParticleSystem::UpdatePairsAndTriads(
	int32 firstIndex, int32 lastIndex,
	b2ParticleGroup* groupA, b2ParticleGroup* groupB)
{
	uint32 particleFlags = 0;
	for (int32 i = firstIndex; i < lastIndex; i++)
	{
		particleFlags |= m_flagsBuffer.data[i];
	}
	if (particleFlags & k_pairFlags)
	{
		for (int32 k = 0; k < m_contactCount; k++)
		{
			const b2ParticleContact& contact = m_contactBuffer[k];
			int32 a = contact.indexA;
			int32 b = contact.indexB;
			if (a > b) b2Swap(a, b);
			if ((groupA->ContainsParticle(a) || groupB->ContainsParticle(b)) &&
				(groupA->ContainsParticle(b) || groupB->ContainsParticle(a)))
			{
				if (m_pairCount >= m_pairCapacity)
				{
					int32 oldCapacity = m_pairCapacity;
					int32 newCapacity = m_pairCount ? 2 * m_pairCount :
						b2_minParticleBufferCapacity;
					m_pairBuffer = ReallocateBuffer(m_pairBuffer, oldCapacity,
													newCapacity);
					m_pairCapacity = newCapacity;
				}
				Pair& pair = m_pairBuffer[m_pairCount];
				pair.indexA = a;
				pair.indexB = b;
				pair.flags = contact.flags;
				pair.strength = b2Min(groupA->m_strength, groupB->m_strength);
				pair.distance = b2Distance(m_positionBuffer.data[a],
										   m_positionBuffer.data[b]);
				m_pairCount++;
			}
		}
	}
	if (particleFlags & k_triadFlags)
	{
		b2VoronoiDiagram diagram(
			&m_world->m_stackAllocator, lastIndex - firstIndex);
		for (int32 i = firstIndex; i < lastIndex; i++)
		{
			if (!(m_flagsBuffer.data[i] & b2_zombieParticle) &&
				(groupA->ContainsParticle(i) || groupB->ContainsParticle(i)))
			{
				diagram.AddGenerator(m_positionBuffer.data[i], i);
			}
		}
		diagram.Generate(GetParticleStride() / 2);
		UpdateTriadsCallback callback;
		callback.system = this;
		callback.groupA = groupA;
		callback.groupB = groupB;
		diagram.GetNodes(callback);
	}
}

void b2ParticleSystem::UpdateTriadsCallback::operator()(int32 a, int32 b,
															  int32 c) const
{
	// Create a triad if it will contain particles from both groups.
	if ((groupA->ContainsParticle(a) || groupA->ContainsParticle(b) ||
		groupA->ContainsParticle(c))  &&
		(groupB->ContainsParticle(a) || groupB->ContainsParticle(b)  ||
		groupB->ContainsParticle(c)))
	{
		uint32 af = system->m_flagsBuffer.data[a];
		uint32 bf = system->m_flagsBuffer.data[b];
		uint32 cf = system->m_flagsBuffer.data[c];
		if (af & bf & cf & k_triadFlags)
		{
			const b2Vec2& pa = system->m_positionBuffer.data[a];
			const b2Vec2& pb = system->m_positionBuffer.data[b];
			const b2Vec2& pc = system->m_positionBuffer.data[c];
			b2Vec2 dab = pa - pb;
			b2Vec2 dbc = pb - pc;
			b2Vec2 dca = pc - pa;
			float32 maxDistanceSquared = b2_maxTriadDistanceSquared *
										 system->m_squaredDiameter;
			if (b2Dot(dab, dab) < maxDistanceSquared &&
				b2Dot(dbc, dbc) < maxDistanceSquared &&
				b2Dot(dca, dca) < maxDistanceSquared)
			{
				if (system->m_triadCount >= system->m_triadCapacity)
				{
					int32 oldCapacity = system->m_triadCapacity;
					int32 newCapacity = system->m_triadCount ?
						2 * system->m_triadCount :
						b2_minParticleBufferCapacity;
					system->m_triadBuffer = system->ReallocateBuffer(
						system->m_triadBuffer, oldCapacity, newCapacity);
					system->m_triadCapacity = newCapacity;
				}
				Triad& triad = system->m_triadBuffer[system->m_triadCount];
				triad.indexA = a;
				triad.indexB = b;
				triad.indexC = c;
				triad.flags = af | bf | cf;
				triad.strength = b2Min(groupA->m_strength, groupB->m_strength);
				b2Vec2 midPoint = (float32) 1 / 3 * (pa + pb + pc);
				triad.pa = pa - midPoint;
				triad.pb = pb - midPoint;
				triad.pc = pc - midPoint;
				triad.ka = -b2Dot(dca, dab);
				triad.kb = -b2Dot(dab, dbc);
				triad.kc = -b2Dot(dbc, dca);
				triad.s = b2Cross(pa, pb) + b2Cross(pb, pc) + b2Cross(pc, pa);
				system->m_triadCount++;
			}
		}
	}
};

// Only called from SolveZombie() or JoinParticleGroups().
void b2ParticleSystem::DestroyParticleGroup(b2ParticleGroup* group)
{
	b2Assert(m_groupCount > 0);
	b2Assert(group);

	if (m_world->m_destructionListener)
	{
		m_world->m_destructionListener->SayGoodbye(group);
	}

	SetParticleGroupFlags(group, 0);
	for (int32 i = group->m_firstIndex; i < group->m_lastIndex; i++)
	{
		m_groupBuffer[i] = NULL;
	}

	if (group->m_prev)
	{
		group->m_prev->m_next = group->m_next;
	}
	if (group->m_next)
	{
		group->m_next->m_prev = group->m_prev;
	}
	if (group == m_groupList)
	{
		m_groupList = group->m_next;
	}

	--m_groupCount;
	group->~b2ParticleGroup();
	m_world->m_blockAllocator.Free(group, sizeof(b2ParticleGroup));
}

void b2ParticleSystem::ComputeWeight()
{
	// calculates the sum of contact-weights for each particle
	// that means dimensionless density
	memset(m_weightBuffer, 0, sizeof(*m_weightBuffer) * m_count);
	for (int32 k = 0; k < m_bodyContactCount; k++)
	{
		const b2ParticleBodyContact& contact = m_bodyContactBuffer[k];
		int32 a = contact.index;
		float32 w = contact.weight;
		m_weightBuffer[a] += w;
	}
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		int32 a = contact.indexA;
		int32 b = contact.indexB;
		float32 w = contact.weight;
		m_weightBuffer[a] += w;
		m_weightBuffer[b] += w;
	}
}

void b2ParticleSystem::ComputeDepth()
{
	b2ParticleContact* contactGroups = (b2ParticleContact*) m_world->
		m_stackAllocator.Allocate(sizeof(b2ParticleContact) * m_contactCount);
	int32 contactGroupsCount = 0;
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		int32 a = contact.indexA;
		int32 b = contact.indexB;
		const b2ParticleGroup* groupA = m_groupBuffer[a];
		const b2ParticleGroup* groupB = m_groupBuffer[b];
		if (groupA && groupA == groupB &&
			(groupA->m_groupFlags & b2_particleGroupNeedsUpdateDepth))
		{
			contactGroups[contactGroupsCount++] = contact;
		}
	}
	b2ParticleGroup** groupsToUpdate = (b2ParticleGroup**) m_world->
		m_stackAllocator.Allocate(sizeof(b2ParticleGroup*) * m_groupCount);
	int32 groupsToUpdateCount = 0;
	for (b2ParticleGroup* group = m_groupList; group; group = group->GetNext())
	{
		if (group->m_groupFlags & b2_particleGroupNeedsUpdateDepth)
		{
			groupsToUpdate[groupsToUpdateCount++] = group;
			SetParticleGroupFlags(group,
								  group->m_groupFlags &
									~b2_particleGroupNeedsUpdateDepth);
			for (int32 i = group->m_firstIndex; i < group->m_lastIndex; i++)
			{
				m_accumulationBuffer[i] = 0;
			}
		}
	}
	b2Assert(m_depthBuffer);
	for (int32 i = 0; i < groupsToUpdateCount; i++)
	{
		const b2ParticleGroup* group = groupsToUpdate[i];
		for (int32 i = group->m_firstIndex; i < group->m_lastIndex; i++)
		{
			float32 w = m_weightBuffer[i];
			m_depthBuffer[i] = w < 0.8f ? 0 : b2_maxFloat;
		}
	}
	// The number of iterations is equal to particle number from the deepest
	// particle to the nearest surface particle, and in general it is smaller
	// than sqrt of total particle number.
	int32 iterationCount = (int32)b2Sqrt((float)m_count);
	for (int32 t = 0; t < iterationCount; t++)
	{
		bool updated = false;
		for (int32 k = 0; k < contactGroupsCount; k++)
		{
			const b2ParticleContact& contact = contactGroups[k];
			int32 a = contact.indexA;
			int32 b = contact.indexB;
			float32 r = 1 - contact.weight;
			float32& ap0 = m_depthBuffer[a];
			float32& bp0 = m_depthBuffer[b];
			float32 ap1 = bp0 + r;
			float32 bp1 = ap0 + r;
			if (ap0 > ap1)
			{
				ap0 = ap1;
				updated = true;
			}
			if (bp0 > bp1)
			{
				bp0 = bp1;
				updated = true;
			}
		}
		if (!updated)
		{
			break;
		}
	}
	for (int32 i = 0; i < groupsToUpdateCount; i++)
	{
		const b2ParticleGroup* group = groupsToUpdate[i];
		for (int32 i = group->m_firstIndex; i < group->m_lastIndex; i++)
		{
			float32& p = m_depthBuffer[i];
			if (p < b2_maxFloat)
			{
				p *= m_particleDiameter;
			}
			else
			{
				p = 0;
			}
		}
	}
	m_world->m_stackAllocator.Free(groupsToUpdate);
	m_world->m_stackAllocator.Free(contactGroups);
}

inline void b2ParticleSystem::AddContact(int32 a, int32 b)
{
	b2Vec2 d = m_positionBuffer.data[b] - m_positionBuffer.data[a];
	float32 distBtParticlesSq = b2Dot(d, d);
	if (distBtParticlesSq < m_squaredDiameter)
	{
		if (m_contactCount >= m_contactCapacity)
		{
			int32 oldCapacity = m_contactCapacity;
			int32 newCapacity = m_contactCount ?
				2 * m_contactCount : b2_minParticleBufferCapacity;
			m_contactBuffer = ReallocateBuffer(m_contactBuffer, oldCapacity,
											   newCapacity);
			m_contactCapacity = newCapacity;
		}
		float32 invD = b2InvSqrt(distBtParticlesSq);
		b2ParticleContact& contact = m_contactBuffer[m_contactCount];
		contact.indexA = a;
		contact.indexB = b;
		contact.flags = m_flagsBuffer.data[a] | m_flagsBuffer.data[b];
		// 1 - distBtParticles / diameter
		contact.weight = 1 - distBtParticlesSq * invD * m_inverseDiameter;
		contact.normal = invD * d;
		m_contactCount++;
	}
}

static bool b2ParticleContactIsZombie(const b2ParticleContact& contact)
{
	return (contact.flags & b2_zombieParticle) == b2_zombieParticle;
}

void b2ParticleSystem::UpdateContacts(bool exceptZombie)
{
	Proxy* beginProxy = m_proxyBuffer;
	Proxy* endProxy = beginProxy + m_proxyCount;
	for (Proxy* proxy = beginProxy; proxy < endProxy; ++proxy)
	{
		int32 i = proxy->index;
		b2Vec2 p = m_positionBuffer.data[i];
		proxy->tag = computeTag(m_inverseDiameter * p.x,
								m_inverseDiameter * p.y);
	}
	std::sort(beginProxy, endProxy);
	m_contactCount = 0;
	for (Proxy *a = beginProxy, *c = beginProxy; a < endProxy; a++)
	{
		uint32 rightTag = computeRelativeTag(a->tag, 1, 0);
		for (Proxy* b = a + 1; b < endProxy; b++)
		{
			if (rightTag < b->tag) break;
			AddContact(a->index, b->index);
		}
		uint32 bottomLeftTag = computeRelativeTag(a->tag, -1, 1);
		for (; c < endProxy; c++)
		{
			if (bottomLeftTag <= c->tag) break;
		}
		uint32 bottomRightTag = computeRelativeTag(a->tag, 1, 1);
		for (Proxy* b = c; b < endProxy; b++)
		{
			if (bottomRightTag < b->tag) break;
			AddContact(a->index, b->index);
		}
	}
	if (exceptZombie)
	{
		b2ParticleContact* lastContact = std::remove_if(
			m_contactBuffer, m_contactBuffer + m_contactCount,
			b2ParticleContactIsZombie);
		m_contactCount = (int32) (lastContact - m_contactBuffer);
	}
}

void b2ParticleSystem::UpdateBodyContacts()
{
	b2AABB aabb;
	aabb.lowerBound.x = +b2_maxFloat;
	aabb.lowerBound.y = +b2_maxFloat;
	aabb.upperBound.x = -b2_maxFloat;
	aabb.upperBound.y = -b2_maxFloat;
	for (int32 i = 0; i < m_count; i++)
	{
		b2Vec2 p = m_positionBuffer.data[i];
		aabb.lowerBound = b2Min(aabb.lowerBound, p);
		aabb.upperBound = b2Max(aabb.upperBound, p);
	}
	aabb.lowerBound.x -= m_particleDiameter;
	aabb.lowerBound.y -= m_particleDiameter;
	aabb.upperBound.x += m_particleDiameter;
	aabb.upperBound.y += m_particleDiameter;
	m_bodyContactCount = 0;

	class UpdateBodyContactsCallback : public b2QueryCallback
	{
		bool ReportFixture(b2Fixture* fixture)
		{
			if (fixture->IsSensor())
			{
				return true;
			}
			const b2Shape* shape = fixture->GetShape();
			b2Body* b = fixture->GetBody();
			b2Vec2 bp = b->GetWorldCenter();
			float32 bm = b->GetMass();
			float32 bI = b->GetInertia() -
						 bm * b->GetLocalCenter().LengthSquared();
			float32 invBm = bm > 0 ? 1 / bm : 0;
			float32 invBI = bI > 0 ? 1 / bI : 0;
			int32 childCount = shape->GetChildCount();
			for (int32 childIndex = 0; childIndex < childCount; childIndex++)
			{
				b2AABB aabb = fixture->GetAABB(childIndex);
				aabb.lowerBound.x -= m_system->m_particleDiameter;
				aabb.lowerBound.y -= m_system->m_particleDiameter;
				aabb.upperBound.x += m_system->m_particleDiameter;
				aabb.upperBound.y += m_system->m_particleDiameter;
				Proxy* beginProxy = m_system->m_proxyBuffer;
				Proxy* endProxy = beginProxy + m_system->m_proxyCount;
				Proxy* firstProxy = std::lower_bound(
					beginProxy, endProxy,
					computeTag(
						m_system->m_inverseDiameter * aabb.lowerBound.x,
						m_system->m_inverseDiameter * aabb.lowerBound.y));
				Proxy* lastProxy = std::upper_bound(
					firstProxy, endProxy,
					computeTag(
						m_system->m_inverseDiameter * aabb.upperBound.x,
						m_system->m_inverseDiameter * aabb.upperBound.y));
				for (Proxy* proxy = firstProxy; proxy != lastProxy; ++proxy)
				{
					int32 a = proxy->index;
					b2Vec2 ap = m_system->m_positionBuffer.data[a];
					if (aabb.lowerBound.x <= ap.x &&
							ap.x <= aabb.upperBound.x &&
						aabb.lowerBound.y <= ap.y &&
							ap.y <= aabb.upperBound.y)
					{
						float32 d;
						b2Vec2 n;
						fixture->ComputeDistance(ap, &d, &n, childIndex);
						if (d < m_system->m_particleDiameter)
						{
							float32 invAm =
								m_system->m_flagsBuffer.data[a] &
								b2_wallParticle ? 0 :
									m_system->GetParticleInvMass();
							b2Vec2 rp = ap - bp;
							float32 rpn = b2Cross(rp, n);
							float32 invM = invAm + invBm + invBI * rpn * rpn;
							if (m_system->m_bodyContactCount >=
								m_system->m_bodyContactCapacity)
							{
								int32 oldCapacity =
									m_system->m_bodyContactCapacity;
								int32 newCapacity =
									m_system->m_bodyContactCount ?
										2 * m_system->m_bodyContactCount :
										b2_minParticleBufferCapacity;
								m_system->m_bodyContactBuffer =
									m_system->ReallocateBuffer(
										m_system->m_bodyContactBuffer,
										oldCapacity, newCapacity);
								m_system->m_bodyContactCapacity = newCapacity;
							}
							b2ParticleBodyContact& contact =
								m_system->m_bodyContactBuffer[
									m_system->m_bodyContactCount];
							contact.index = a;
							contact.body = b;
							contact.fixture = fixture;
							contact.weight = 
								1 - d * m_system->m_inverseDiameter;
							contact.normal = -n;
							contact.mass = invM > 0 ? 1 / invM : 0;
							m_system->m_bodyContactCount++;
						}
					}
				}
			}
			return true;
		}

		b2ParticleSystem* m_system;

	public:
		UpdateBodyContactsCallback(b2ParticleSystem* system)
		{
			m_system = system;
		}
	} callback(this);

	m_world->QueryAABB(&callback, aabb);

	if (m_strictContactCheck)
	{
		RemoveSpuriousBodyContacts();
	}
}

void b2ParticleSystem::RemoveSpuriousBodyContacts()
{
	// At this point we have a list of contact candidates based on AABB
	// overlap.The AABB query that  generated this returns all collidable
	// fixtures overlapping particle bounding boxes.  This breaks down around
	// vertices where two shapes intersect, such as a "ground" surface made
	// of multiple b2PolygonShapes; it potentially applies a lot of spurious
	// impulses from normals that should not actually contribute.  See the
	// Ramp example in Testbed.
	//
	// To correct for this, we apply this algorithm:
	//   * sort contacts by particle and subsort by weight (nearest to farthest)
	//   * for each contact per particle:
	//      - project a point at the contact distance along the inverse of the
	//        contact normal
	//      - if this intersects the fixture that generated the contact, apply
	//         it, otherwise discard as impossible
	//      - repeat for up to n nearest contacts, currently we get good results
	//        from n=3.
	std::sort(m_bodyContactBuffer, m_bodyContactBuffer + m_bodyContactCount,
			  b2ParticleSystem::BodyContactCompare);

	int32 discarded = 0;
	std::remove_if(m_bodyContactBuffer, m_bodyContactBuffer + m_bodyContactCount,
				   b2ParticleBodyContactRemovePredicate(this, &discarded));

	m_bodyContactCount -= discarded;
}

bool b2ParticleSystem::BodyContactCompare(const b2ParticleBodyContact &lhs,
										  const b2ParticleBodyContact &rhs)
{
	if (lhs.index == rhs.index)
	{
		// Subsort by weight, decreasing.
		return lhs.weight > rhs.weight;
	}
	return lhs.index < rhs.index;
}


void b2ParticleSystem::SolveCollision(const b2TimeStep& step)
{
	// This function detects particles which are crossing boundary of bodies
	// and modifies velocities of them so that they will move just in front of
	// boundary. This function function also applies the reaction force to
	// bodies as precisely as the numerical stability is kept.
	b2AABB aabb;
	aabb.lowerBound.x = +b2_maxFloat;
	aabb.lowerBound.y = +b2_maxFloat;
	aabb.upperBound.x = -b2_maxFloat;
	aabb.upperBound.y = -b2_maxFloat;
	for (int32 i = 0; i < m_count; i++)
	{
		b2Vec2 v = m_velocityBuffer.data[i];
		b2Vec2 p1 = m_positionBuffer.data[i];
		b2Vec2 p2 = p1 + step.dt * v;
		aabb.lowerBound = b2Min(aabb.lowerBound, b2Min(p1, p2));
		aabb.upperBound = b2Max(aabb.upperBound, b2Max(p1, p2));
	}
	class SolveCollisionCallback : public b2QueryCallback
	{
		bool ReportFixture(b2Fixture* fixture)
		{
			if (fixture->IsSensor())
			{
				return true;
			}
			const b2Shape* shape = fixture->GetShape();
			b2Body* body = fixture->GetBody();
			Proxy* beginProxy = m_system->m_proxyBuffer;
			Proxy* endProxy = beginProxy + m_system->m_proxyCount;
			int32 childCount = shape->GetChildCount();
			bool limitBodyVelocity = false;
			for (int32 childIndex = 0; childIndex < childCount; childIndex++)
			{
				b2AABB aabb = fixture->GetAABB(childIndex);
				aabb.lowerBound.x -= m_system->m_particleDiameter;
				aabb.lowerBound.y -= m_system->m_particleDiameter;
				aabb.upperBound.x += m_system->m_particleDiameter;
				aabb.upperBound.y += m_system->m_particleDiameter;
				Proxy* firstProxy = std::lower_bound(
					beginProxy, endProxy,
					computeTag(
						m_system->m_inverseDiameter * aabb.lowerBound.x,
						m_system->m_inverseDiameter * aabb.lowerBound.y));
				Proxy* lastProxy = std::upper_bound(
					firstProxy, endProxy,
					computeTag(
						m_system->m_inverseDiameter * aabb.upperBound.x,
						m_system->m_inverseDiameter * aabb.upperBound.y));
				for (Proxy* proxy = firstProxy; proxy != lastProxy; ++proxy)
				{
					int32 a = proxy->index;
					b2Vec2 ap = m_system->m_positionBuffer.data[a];
					if (aabb.lowerBound.x <= ap.x &&
							ap.x <= aabb.upperBound.x &&
						aabb.lowerBound.y <= ap.y &&
							ap.y <= aabb.upperBound.y)
					{
						b2Vec2 av = m_system->m_velocityBuffer.data[a];
						b2RayCastOutput output;
						b2RayCastInput input;
						if (m_system->m_iterationIndex == 0)
						{
							input.p1 = b2Mul(body->m_xf,
											 b2MulT(body->m_xf0, ap));
						}
						else
						{
							input.p1 = ap;
						}
						input.p2 = ap + m_step.dt * av;
						input.maxFraction = 1;
						if (fixture->RayCast(&output, input, childIndex))
						{
							b2Vec2 p =
								(1 - output.fraction) * input.p1 +
								output.fraction * input.p2 +
								b2_linearSlop * output.normal;
							b2Vec2 v = m_step.inv_dt * (p - ap);
							m_system->m_velocityBuffer.data[a] = v;
							b2Vec2 f = m_system->GetParticleMass() * (av - v);
							f = b2Dot(f, output.normal) * output.normal;
							// If density of the body is smaller than particle,
							// the reactive force to it will be discounted.
							float32 densityRatio =
								fixture->GetDensity() *
								m_system->m_inverseDensity;
							if (densityRatio < 1)
							{
								f *= densityRatio;
							}
							body->ApplyLinearImpulse(f, p, true);
							limitBodyVelocity = true;
						}
					}
				}
			}
			if (limitBodyVelocity)
			{
				b2Vec2 lc = body->GetLocalCenter();
				float32 m = body->GetMass();
				float32 I = body->GetInertia() - m * b2Dot(lc, lc);
				b2Vec2 v = body->GetLinearVelocity();
				float32 w = body->GetAngularVelocity();
				float32 E = 0.5f * m * b2Dot(v, v) + 0.5f * I * w * w;
				float32 E0 = m * m_system->GetCriticalVelocitySquared(m_step);
				if (E > E0)
				{
					float32 s = E0 / E;
					body->SetLinearVelocity(s * v);
					body->SetAngularVelocity(s * w);
				}
			}
			return true;
		}

		b2ParticleSystem* m_system;
		b2TimeStep m_step;

	public:
		SolveCollisionCallback(b2ParticleSystem* system,
							   const b2TimeStep& step)
		{
			m_system = system;
			m_step = step;
		}
	} callback(this, step);
	m_world->QueryAABB(&callback, aabb);
}

void b2ParticleSystem::SolveBarrier(const b2TimeStep& step)
{
	// If a particle is passing between paired barrier particles,
	// its velocity will be decelerated to avoid passing.
	for (int32 i = 0; i < m_count; i++)
	{
		uint32 flags = m_flagsBuffer.data[i];
		if (flags & b2_barrierParticle)
		{
			if (flags & b2_wallParticle)
			{
				m_velocityBuffer.data[i].SetZero();
				continue;
			}
			const b2ParticleGroup* group = m_groupBuffer[i];
			if (group->m_groupFlags & b2_rigidParticleGroup)
			{
				m_velocityBuffer.data[i] =
					group->GetLinearVelocity() +
					b2Cross(
						group->GetAngularVelocity(),
						m_positionBuffer.data[i] - group->GetCenter());
			}
		}
	}
	Proxy* beginProxy = m_proxyBuffer;
	Proxy* endProxy = beginProxy + m_proxyCount;
	float32 tmax = b2_barrierCollisionTime * step.dt;
	for (int32 k = 0; k < m_pairCount; k++)
	{
		const Pair& pair = m_pairBuffer[k];
		if (pair.flags & b2_barrierParticle)
		{
			int32 a = pair.indexA;
			int32 b = pair.indexB;
			b2Vec2 pa = m_positionBuffer.data[a];
			b2Vec2 pb = m_positionBuffer.data[b];
			b2Vec2 lower = b2Min(pa, pb);
			b2Vec2 upper = b2Max(pa, pb);
			lower.x -= m_particleDiameter;
			lower.y -= m_particleDiameter;
			upper.x += m_particleDiameter;
			upper.y += m_particleDiameter;
			Proxy* firstProxy = std::lower_bound(
				beginProxy, endProxy,
				computeTag(
					m_inverseDiameter * lower.x,
					m_inverseDiameter * lower.y));
			Proxy* lastProxy = std::upper_bound(
				firstProxy, endProxy,
				computeTag(
					m_inverseDiameter * upper.x,
					m_inverseDiameter * upper.y));
			b2Vec2 va = m_velocityBuffer.data[a];
			b2Vec2 vb = m_velocityBuffer.data[b];
			b2Vec2 pba = pb - pa;
			b2Vec2 vba = vb - va;
			for (Proxy* proxy = firstProxy; proxy != lastProxy; ++proxy)
			{
				int32 c = proxy->index;
				b2Vec2 pc = m_positionBuffer.data[c];
				if (lower.x <= pc.x && pc.x <= upper.x &&
					lower.y <= pc.y && pc.y <= upper.y &&
					m_groupBuffer[a] != m_groupBuffer[c] &&
					m_groupBuffer[b] != m_groupBuffer[c])
				{
					b2Vec2& vc = m_velocityBuffer.data[c];
					// Solve the equation below:
					//   (1-s)*(pa+t*va)+s*(pb+t*vb) = pc+t*vc
					// which expresses that the particle c will pass a line
					// connecting the particles a and b at the time of t.
					// if s is between 0 and 1, c will pass between a and b.
					b2Vec2 pca = pc - pa;
					b2Vec2 vca = vc - va;
					float32 e2 = b2Cross(vba, vca);
					float32 e1 = b2Cross(pba, vca) - b2Cross(pca, vba);
					float32 e0 = b2Cross(pba, pca);
					float32 s, t;
					b2Vec2 qba, qca;
					if (e2 == 0)
					{
						if (e1 == 0) continue;
						t = - e0 / e1;
						if (t < 0 || t > tmax) continue;
						qba = pba + t * vba;
						qca = pca + t * vca;
						s = b2Dot(qba, qca) / b2Dot(qba, qba);
						if (s < 0 || s > 1) continue;
					}
					else
					{
						float32 det = e1 * e1 - 4 * e0 * e2;
						if (det < 0) continue;
						float32 sqrtDet = b2Sqrt(det);
						float32 t1 = (- e1 - sqrtDet) / (2 * e2);
						float32 t2 = (- e1 + sqrtDet) / (2 * e2);
						if (t1 > t2) b2Swap(t1, t2);
						t = t1;
						qba = pba + t * vba;
						qca = pca + t * vca;
						s = b2Dot(qba, qca) / b2Dot(qba, qba);
						if (t < 0 || t > tmax || s < 0 || s > 1)
						{
							t = t2;
							if (t < 0 || t > tmax) continue;
							qba = pba + t * vba;
							qca = pca + t * vca;
							s = b2Dot(qba, qca) / b2Dot(qba, qba);
							if (s < 0 || s > 1) continue;
						}
					}
					vc = va + s * vba;
				}
			}
		}
	}
}

void b2ParticleSystem::Solve(const b2TimeStep& step)
{
	if (m_count == 0)
	{
		return;
	}
	if (m_allParticleFlags & b2_zombieParticle)
	{
		SolveZombie();
	}
	if (m_needsUpdateAllParticleFlags)
	{
		UpdateAllParticleFlags();
	}
	if (m_needsUpdateAllGroupFlags)
	{
		UpdateAllGroupFlags();
	}
	for (m_iterationIndex = 0;
		m_iterationIndex < step.particleIterations;
		m_iterationIndex++)
	{
		++m_timestamp;
		b2TimeStep subStep = step;
		subStep.dt /= step.particleIterations;
		subStep.inv_dt *= step.particleIterations;
		UpdateBodyContacts();
		UpdateContacts(false);
		ComputeWeight();
		if (m_allGroupFlags & b2_particleGroupNeedsUpdateDepth)
		{
			ComputeDepth();
		}
		if (m_allParticleFlags & b2_viscousParticle)
		{
			SolveViscous();
		}
		if (m_allParticleFlags & b2_powderParticle)
		{
			SolvePowder(subStep);
		}
		if (m_allParticleFlags & b2_tensileParticle)
		{
			SolveTensile(subStep);
		}
		if (m_allGroupFlags & b2_solidParticleGroup)
		{
			SolveSolid(subStep);
		}
		if (m_allParticleFlags & b2_colorMixingParticle)
		{
			SolveColorMixing();
		}
		SolveGravity(subStep);
		if (m_allParticleFlags & b2_staticPressureParticle)
		{
			SolveStaticPressure(subStep);
		}
		SolvePressure(subStep);
		SolveDamping(subStep);
		if (m_allParticleFlags & k_extraDampingFlags)
		{
			SolveExtraDamping();
		}
		// SolveElastic and SolveSpring refer the current velocities for
		// numerical stability, they should be called as late as possible.
		if (m_allParticleFlags & b2_elasticParticle)
		{
			SolveElastic(subStep);
		}
		if (m_allParticleFlags & b2_springParticle)
		{
			SolveSpring(subStep);
		}
		LimitVelocity(subStep);
		if (m_allParticleFlags & b2_barrierParticle)
		{
			SolveBarrier(subStep);
		}
		// SolveCollision, SolveRigid and SolveWall should be called after
		// other force functions because they may require particles to have
		// specific velocities.
		SolveCollision(subStep);
		if (m_allGroupFlags & b2_rigidParticleGroup)
		{
			SolveRigid(subStep);
		}
		if (m_allParticleFlags & b2_wallParticle)
		{
			SolveWall();
		}
		// The particle positions can be updated only at the end of substep.
		for (int32 i = 0; i < m_count; i++)
		{
			m_positionBuffer.data[i] += subStep.dt * m_velocityBuffer.data[i];
		}
	}
}

void b2ParticleSystem::UpdateAllParticleFlags()
{
	m_allParticleFlags = 0;
	for (int32 i = 0; i < m_count; i++)
	{
		m_allParticleFlags |= m_flagsBuffer.data[i];
	}
	m_needsUpdateAllParticleFlags = false;
}

void b2ParticleSystem::UpdateAllGroupFlags()
{
	m_allGroupFlags = 0;
	for (const b2ParticleGroup* group = m_groupList; group;
		 group = group->GetNext())
	{
		m_allGroupFlags |= group->m_groupFlags;
	}
	m_needsUpdateAllGroupFlags = false;
}

void b2ParticleSystem::LimitVelocity(const b2TimeStep& step)
{
	float32 criticalVelocitySquared = GetCriticalVelocitySquared(step);
	for (int32 i = 0; i < m_count; i++)
	{
		b2Vec2& v = m_velocityBuffer.data[i];
		float32 v2 = b2Dot(v, v);
		if (v2 > criticalVelocitySquared)
		{
			v *= b2Sqrt(criticalVelocitySquared / v2);
		}
	}
}

void b2ParticleSystem::SolveGravity(const b2TimeStep& step)
{
	b2Vec2 gravity = step.dt * m_gravityScale * m_world->GetGravity();
	for (int32 i = 0; i < m_count; i++)
	{
		m_velocityBuffer.data[i] += gravity;
	}
}

void b2ParticleSystem::SolveStaticPressure(const b2TimeStep& step)
{
	m_staticPressureBuffer = RequestParticleBuffer(m_staticPressureBuffer);
	float32 criticalPressure = GetCriticalPressure(step);
	float32 pressurePerWeight = m_def.staticPressureStrength * criticalPressure;
	float32 maxPressure = b2_maxParticlePressure * criticalPressure;
	float32 relaxation = m_def.staticPressureRelaxation;
	/// Compute pressure satisfying the modified Poisson equation:
	///     Sum_for_j((p_i - p_j) * w_ij) + relaxation * p_i =
	///     pressurePerWeight * (w_i - b2_minParticleWeight)
	/// by iterating the calculation:
	///     p_i = (Sum_for_j(p_j * w_ij) + pressurePerWeight *
	///           (w_i - b2_minParticleWeight)) / (w_i + relaxation)
	/// where
	///     p_i and p_j are static pressure of particle i and j
	///     w_ij is contact weight between particle i and j
	///     w_i is sum of contact weight of particle i
	for (int32 t = 0; t < m_def.staticPressureIterations; t++)
	{
		memset(m_accumulationBuffer, 0,
			   sizeof(*m_accumulationBuffer) * m_count);
		for (int32 k = 0; k < m_contactCount; k++)
		{
			const b2ParticleContact& contact = m_contactBuffer[k];
			if (contact.flags & b2_staticPressureParticle)
			{
				int32 a = contact.indexA;
				int32 b = contact.indexB;
				float32 w = contact.weight;
				m_accumulationBuffer[a] +=
					w * m_staticPressureBuffer[b]; // a <- b
				m_accumulationBuffer[b] +=
					w * m_staticPressureBuffer[a]; // b <- a
			}
		}
		for (int32 i = 0; i < m_count; i++)
		{
			float32 w = m_weightBuffer[i];
			if (m_flagsBuffer.data[i] & b2_staticPressureParticle)
			{
				float32 wh = m_accumulationBuffer[i];
				float32 h =
					(wh + pressurePerWeight * (w - b2_minParticleWeight)) /
					(w + relaxation);
				m_staticPressureBuffer[i] = b2Clamp(h, 0.0f, maxPressure);
			}
			else
			{
				m_staticPressureBuffer[i] = 0;
			}
		}
	}
}

void b2ParticleSystem::SolvePressure(const b2TimeStep& step)
{
	// calculates pressure as a linear function of density
	float32 criticalPressure = GetCriticalPressure(step);
	float32 pressurePerWeight = m_def.pressureStrength * criticalPressure;
	float32 maxPressure = b2_maxParticlePressure * criticalPressure;
	for (int32 i = 0; i < m_count; i++)
	{
		float32 w = m_weightBuffer[i];
		float32 h = pressurePerWeight * b2Max(0.0f, w - b2_minParticleWeight);
		m_accumulationBuffer[i] = b2Min(h, maxPressure);
	}
	// ignores particles which have their own repulsive force
	if (m_allParticleFlags & k_noPressureFlags)
	{
		for (int32 i = 0; i < m_count; i++)
		{
			if (m_flagsBuffer.data[i] & k_noPressureFlags)
			{
				m_accumulationBuffer[i] = 0;
			}
		}
	}
	// static pressure
	if (m_allParticleFlags & b2_staticPressureParticle)
	{
		b2Assert(m_staticPressureBuffer);
		for (int32 i = 0; i < m_count; i++)
		{
			if (m_flagsBuffer.data[i] & b2_staticPressureParticle)
			{
				m_accumulationBuffer[i] += m_staticPressureBuffer[i];
			}
		}
	}
	// applies pressure between each particles in contact
	float32 velocityPerPressure = step.dt / (m_density * m_particleDiameter);
	for (int32 k = 0; k < m_bodyContactCount; k++)
	{
		const b2ParticleBodyContact& contact = m_bodyContactBuffer[k];
		int32 a = contact.index;
		b2Body* b = contact.body;
		float32 w = contact.weight;
		float32 m = contact.mass;
		b2Vec2 n = contact.normal;
		b2Vec2 p = m_positionBuffer.data[a];
		float32 h = m_accumulationBuffer[a] + pressurePerWeight * w;
		b2Vec2 f = velocityPerPressure * w * m * h * n;
		m_velocityBuffer.data[a] -= GetParticleInvMass() * f;
		b->ApplyLinearImpulse(f, p, true);
	}
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		int32 a = contact.indexA;
		int32 b = contact.indexB;
		float32 w = contact.weight;
		b2Vec2 n = contact.normal;
		float32 h = m_accumulationBuffer[a] + m_accumulationBuffer[b];
		b2Vec2 f = velocityPerPressure * w * h * n;
		m_velocityBuffer.data[a] -= f;
		m_velocityBuffer.data[b] += f;
	}
}

void b2ParticleSystem::SolveDamping(const b2TimeStep& step)
{
	// reduces normal velocity of each contact
	float32 linearDamping = m_def.dampingStrength;
	float32 quadraticDamping = 1 / GetCriticalVelocity(step);
	for (int32 k = 0; k < m_bodyContactCount; k++)
	{
		const b2ParticleBodyContact& contact = m_bodyContactBuffer[k];
		int32 a = contact.index;
		b2Body* b = contact.body;
		float32 w = contact.weight;
		float32 m = contact.mass;
		b2Vec2 n = contact.normal;
		b2Vec2 p = m_positionBuffer.data[a];
		b2Vec2 v = b->GetLinearVelocityFromWorldPoint(p) -
				   m_velocityBuffer.data[a];
		float32 vn = b2Dot(v, n);
		if (vn < 0)
		{
			float32 damping =
				b2Max(linearDamping * w, b2Min(- quadraticDamping * vn, 0.5f));
			b2Vec2 f = damping * m * vn * n;
			m_velocityBuffer.data[a] += GetParticleInvMass() * f;
			b->ApplyLinearImpulse(-f, p, true);
		}
	}
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		int32 a = contact.indexA;
		int32 b = contact.indexB;
		float32 w = contact.weight;
		b2Vec2 n = contact.normal;
		b2Vec2 v = m_velocityBuffer.data[b] - m_velocityBuffer.data[a];
		float32 vn = b2Dot(v, n);
		if (vn < 0)
		{
			float32 damping =
				b2Max(linearDamping * w, b2Min(- quadraticDamping * vn, 0.5f));
			b2Vec2 f = damping * vn * n;
			m_velocityBuffer.data[a] += f;
			m_velocityBuffer.data[b] -= f;
		}
	}
}

void b2ParticleSystem::SolveExtraDamping()
{
	// Applies additional damping force between bodies and particles which can
	// produce strong repulsive force. Applying damping force multiple times
	// is effective in suppressing vibration.
	for (int32 k = 0; k < m_bodyContactCount; k++)
	{
		const b2ParticleBodyContact& contact = m_bodyContactBuffer[k];
		int32 a = contact.index;
		if (m_flagsBuffer.data[a] & k_extraDampingFlags)
		{
			b2Body* b = contact.body;
			float32 m = contact.mass;
			b2Vec2 n = contact.normal;
			b2Vec2 p = m_positionBuffer.data[a];
			b2Vec2 v =
				b->GetLinearVelocityFromWorldPoint(p) -
				m_velocityBuffer.data[a];
			float32 vn = b2Dot(v, n);
			if (vn < 0)
			{
				b2Vec2 f = 0.5f * m * vn * n;
				m_velocityBuffer.data[a] += GetParticleInvMass() * f;
				b->ApplyLinearImpulse(-f, p, true);
			}
		}
	}
}

void b2ParticleSystem::SolveWall()
{
	for (int32 i = 0; i < m_count; i++)
	{
		if (m_flagsBuffer.data[i] & b2_wallParticle)
		{
			m_velocityBuffer.data[i].SetZero();
		}
	}
}

void b2ParticleSystem::SolveRigid(const b2TimeStep& step)
{
	for (b2ParticleGroup* group = m_groupList; group; group = group->GetNext())
	{
		if (group->m_groupFlags & b2_rigidParticleGroup)
		{
			group->UpdateStatistics();
			b2Rot rotation(step.dt * group->m_angularVelocity);
			b2Transform transform(
				group->m_center + step.dt * group->m_linearVelocity -
				b2Mul(rotation, group->m_center), rotation);
			group->m_transform = b2Mul(transform, group->m_transform);
			b2Transform velocityTransform;
			velocityTransform.p.x = step.inv_dt * transform.p.x;
			velocityTransform.p.y = step.inv_dt * transform.p.y;
			velocityTransform.q.s = step.inv_dt * transform.q.s;
			velocityTransform.q.c = step.inv_dt * (transform.q.c - 1);
			for (int32 i = group->m_firstIndex; i < group->m_lastIndex; i++)
			{
				m_velocityBuffer.data[i] = b2Mul(velocityTransform,
												 m_positionBuffer.data[i]);
			}
		}
	}
}

void b2ParticleSystem::SolveElastic(const b2TimeStep& step)
{
	float32 elasticStrength = step.inv_dt * m_def.elasticStrength;
	for (int32 k = 0; k < m_triadCount; k++)
	{
		const Triad& triad = m_triadBuffer[k];
		if (triad.flags & b2_elasticParticle)
		{
			int32 a = triad.indexA;
			int32 b = triad.indexB;
			int32 c = triad.indexC;
			const b2Vec2& oa = triad.pa;
			const b2Vec2& ob = triad.pb;
			const b2Vec2& oc = triad.pc;
			b2Vec2 pa = m_positionBuffer.data[a];
			b2Vec2 pb = m_positionBuffer.data[b];
			b2Vec2 pc = m_positionBuffer.data[c];
			b2Vec2& va = m_velocityBuffer.data[a];
			b2Vec2& vb = m_velocityBuffer.data[b];
			b2Vec2& vc = m_velocityBuffer.data[c];
			pa += step.dt * va;
			pb += step.dt * vb;
			pc += step.dt * vc;
			b2Vec2 midPoint = (float32) 1 / 3 * (pa + pb + pc);
			pa -= midPoint;
			pb -= midPoint;
			pc -= midPoint;
			b2Rot r;
			r.s = b2Cross(oa, pa) + b2Cross(ob, pb) + b2Cross(oc, pc);
			r.c = b2Dot(oa, pa) + b2Dot(ob, pb) + b2Dot(oc, pc);
			float32 r2 = r.s * r.s + r.c * r.c;
			float32 invR = b2InvSqrt(r2);
			r.s *= invR;
			r.c *= invR;
			float32 strength = elasticStrength * triad.strength;
			va += strength * (b2Mul(r, oa) - pa);
			vb += strength * (b2Mul(r, ob) - pb);
			vc += strength * (b2Mul(r, oc) - pc);
		}
	}
}

void b2ParticleSystem::SolveSpring(const b2TimeStep& step)
{
	float32 springStrength = step.inv_dt * m_def.springStrength;
	for (int32 k = 0; k < m_pairCount; k++)
	{
		const Pair& pair = m_pairBuffer[k];
		if (pair.flags & b2_springParticle)
		{
			int32 a = pair.indexA;
			int32 b = pair.indexB;
			b2Vec2 pa = m_positionBuffer.data[a];
			b2Vec2 pb = m_positionBuffer.data[b];
			b2Vec2& va = m_velocityBuffer.data[a];
			b2Vec2& vb = m_velocityBuffer.data[b];
			pa += step.dt * va;
			pb += step.dt * vb;
			b2Vec2 d = pb - pa;
			float32 r0 = pair.distance;
			float32 r1 = d.Length();
			float32 strength = springStrength * pair.strength;
			b2Vec2 f = strength * (r0 - r1) / r1 * d;
			va -= f;
			vb += f;
		}
	}
}

void b2ParticleSystem::SolveTensile(const b2TimeStep& step)
{
	b2Assert(m_accumulation2Buffer);
	for (int32 i = 0; i < m_count; i++)
	{
		m_accumulation2Buffer[i] = b2Vec2_zero;
	}
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		if (contact.flags & b2_tensileParticle)
		{
			int32 a = contact.indexA;
			int32 b = contact.indexB;
			float32 w = contact.weight;
			b2Vec2 n = contact.normal;
			b2Vec2 weightedNormal = (1 - w) * w * n;
			m_accumulation2Buffer[a] -= weightedNormal;
			m_accumulation2Buffer[b] += weightedNormal;
		}
	}
	float32 criticalVelocity = GetCriticalVelocity(step);
	float32 pressureStrength = m_def.surfaceTensionPressureStrength
							 * criticalVelocity;
	float32 normalStrength = m_def.surfaceTensionNormalStrength
						   * criticalVelocity;
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		if (contact.flags & b2_tensileParticle)
		{
			int32 a = contact.indexA;
			int32 b = contact.indexB;
			float32 w = contact.weight;
			b2Vec2 n = contact.normal;
			float32 h = m_weightBuffer[a] + m_weightBuffer[b];
			b2Vec2 s = m_accumulation2Buffer[b] - m_accumulation2Buffer[a];
			float32 fn = (pressureStrength * (h - 2)
					      + normalStrength * b2Dot(s, n)) * w;
			b2Vec2 f = fn * n;
			m_velocityBuffer.data[a] -= f;
			m_velocityBuffer.data[b] += f;
		}
	}
}

void b2ParticleSystem::SolveViscous()
{
	float32 viscousStrength = m_def.viscousStrength;
	for (int32 k = 0; k < m_bodyContactCount; k++)
	{
		const b2ParticleBodyContact& contact = m_bodyContactBuffer[k];
		int32 a = contact.index;
		if (m_flagsBuffer.data[a] & b2_viscousParticle)
		{
			b2Body* b = contact.body;
			float32 w = contact.weight;
			float32 m = contact.mass;
			b2Vec2 p = m_positionBuffer.data[a];
			b2Vec2 v = b->GetLinearVelocityFromWorldPoint(p) -
					   m_velocityBuffer.data[a];
			b2Vec2 f = viscousStrength * m * w * v;
			m_velocityBuffer.data[a] += GetParticleInvMass() * f;
			b->ApplyLinearImpulse(-f, p, true);
		}
	}
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		if (contact.flags & b2_viscousParticle)
		{
			int32 a = contact.indexA;
			int32 b = contact.indexB;
			float32 w = contact.weight;
			b2Vec2 v = m_velocityBuffer.data[b] - m_velocityBuffer.data[a];
			b2Vec2 f = viscousStrength * w * v;
			m_velocityBuffer.data[a] += f;
			m_velocityBuffer.data[b] -= f;
		}
	}
}

void b2ParticleSystem::SolvePowder(const b2TimeStep& step)
{
	float32 powderStrength = m_def.powderStrength * GetCriticalVelocity(step);
	float32 minWeight = 1.0f - b2_particleStride;
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		if (contact.flags & b2_powderParticle)
		{
			float32 w = contact.weight;
			if (w > minWeight)
			{
				int32 a = contact.indexA;
				int32 b = contact.indexB;
				b2Vec2 n = contact.normal;
				b2Vec2 f = powderStrength * (w - minWeight) * n;
				m_velocityBuffer.data[a] -= f;
				m_velocityBuffer.data[b] += f;
			}
		}
	}
}

void b2ParticleSystem::SolveSolid(const b2TimeStep& step)
{
	// applies extra repulsive force from solid particle groups
	b2Assert(m_depthBuffer);
	float32 ejectionStrength = step.inv_dt * m_def.ejectionStrength;
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		int32 a = contact.indexA;
		int32 b = contact.indexB;
		if (m_groupBuffer[a] != m_groupBuffer[b])
		{
			float32 w = contact.weight;
			b2Vec2 n = contact.normal;
			float32 h = m_depthBuffer[a] + m_depthBuffer[b];
			b2Vec2 f = ejectionStrength * h * w * n;
			m_velocityBuffer.data[a] -= f;
			m_velocityBuffer.data[b] += f;
		}
	}
}

void b2ParticleSystem::SolveColorMixing()
{
	// mixes color between contacting particles
	b2Assert(m_colorBuffer.data);
	const int32 colorMixing128 = (int32) (128 * m_def.colorMixingStrength);
	if (colorMixing128) {
		for (int32 k = 0; k < m_contactCount; k++)
		{
			const b2ParticleContact& contact = m_contactBuffer[k];
			int32 a = contact.indexA;
			int32 b = contact.indexB;
			if (m_flagsBuffer.data[a] & m_flagsBuffer.data[b] &
				b2_colorMixingParticle)
			{
				b2ParticleColor& colorA = m_colorBuffer.data[a];
				b2ParticleColor& colorB = m_colorBuffer.data[b];
				// Use the static method to ensure certain compilers inline
				// this correctly.
				b2ParticleColor::MixColors(&colorA, &colorB, colorMixing128);
			}
		}
	}
}

void b2ParticleSystem::SolveZombie()
{
	// removes particles with zombie flag
	int32 newCount = 0;
	int32* newIndices = (int32*) m_world->m_stackAllocator.Allocate(
		sizeof(int32) * m_count);
	uint32 allParticleFlags = 0;
	for (int32 i = 0; i < m_count; i++)
	{
		int32 flags = m_flagsBuffer.data[i];
		if (flags & b2_zombieParticle)
		{
			b2DestructionListener * const destructionListener =
				m_world->m_destructionListener;
			if ((flags & b2_destructionListener) &&
				destructionListener)
			{
				destructionListener->SayGoodbye(i);
			}
			newIndices[i] = b2_invalidParticleIndex;
		}
		else
		{
			newIndices[i] = newCount;
			if (i != newCount)
			{
				m_flagsBuffer.data[newCount] = m_flagsBuffer.data[i];
				m_positionBuffer.data[newCount] = m_positionBuffer.data[i];
				m_velocityBuffer.data[newCount] = m_velocityBuffer.data[i];
				m_groupBuffer[newCount] = m_groupBuffer[i];
				if (m_staticPressureBuffer)
				{
					m_staticPressureBuffer[newCount] =
						m_staticPressureBuffer[i];
				}
				if (m_depthBuffer)
				{
					m_depthBuffer[newCount] = m_depthBuffer[i];
				}
				if (m_colorBuffer.data)
				{
					m_colorBuffer.data[newCount] = m_colorBuffer.data[i];
				}
				if (m_userDataBuffer.data)
				{
					m_userDataBuffer.data[newCount] = m_userDataBuffer.data[i];
				}
			}
			newCount++;
			allParticleFlags |= flags;
		}
	}

	// predicate functions
	struct Test
	{
		static bool IsProxyInvalid(const Proxy& proxy)
		{
			return proxy.index < 0;
		}
		static bool IsContactInvalid(const b2ParticleContact& contact)
		{
			return contact.indexA < 0 || contact.indexB < 0;
		}
		static bool IsBodyContactInvalid(const b2ParticleBodyContact& contact)
		{
			return contact.index < 0;
		}
		static bool IsPairInvalid(const Pair& pair)
		{
			return pair.indexA < 0 || pair.indexB < 0;
		}
		static bool IsTriadInvalid(const Triad& triad)
		{
			return triad.indexA < 0 || triad.indexB < 0 || triad.indexC < 0;
		}
	};

	// update proxies
	for (int32 k = 0; k < m_proxyCount; k++)
	{
		Proxy& proxy = m_proxyBuffer[k];
		proxy.index = newIndices[proxy.index];
	}
	Proxy* lastProxy = std::remove_if(
		m_proxyBuffer, m_proxyBuffer + m_proxyCount,
		Test::IsProxyInvalid);
	m_proxyCount = (int32) (lastProxy - m_proxyBuffer);

	// update contacts
	for (int32 k = 0; k < m_contactCount; k++)
	{
		b2ParticleContact& contact = m_contactBuffer[k];
		contact.indexA = newIndices[contact.indexA];
		contact.indexB = newIndices[contact.indexB];
	}
	b2ParticleContact* lastContact = std::remove_if(
		m_contactBuffer, m_contactBuffer + m_contactCount,
		Test::IsContactInvalid);
	m_contactCount = (int32) (lastContact - m_contactBuffer);

	// update particle-body contacts
	for (int32 k = 0; k < m_bodyContactCount; k++)
	{
		b2ParticleBodyContact& contact = m_bodyContactBuffer[k];
		contact.index = newIndices[contact.index];
	}
	b2ParticleBodyContact* lastBodyContact = std::remove_if(
		m_bodyContactBuffer, m_bodyContactBuffer + m_bodyContactCount,
		Test::IsBodyContactInvalid);
	m_bodyContactCount = (int32) (lastBodyContact - m_bodyContactBuffer);

	// update pairs
	for (int32 k = 0; k < m_pairCount; k++)
	{
		Pair& pair = m_pairBuffer[k];
		pair.indexA = newIndices[pair.indexA];
		pair.indexB = newIndices[pair.indexB];
	}
	Pair* lastPair = std::remove_if(
		m_pairBuffer, m_pairBuffer + m_pairCount, Test::IsPairInvalid);
	m_pairCount = (int32) (lastPair - m_pairBuffer);

	// update triads
	for (int32 k = 0; k < m_triadCount; k++)
	{
		Triad& triad = m_triadBuffer[k];
		triad.indexA = newIndices[triad.indexA];
		triad.indexB = newIndices[triad.indexB];
		triad.indexC = newIndices[triad.indexC];
	}
	Triad* lastTriad = std::remove_if(
		m_triadBuffer, m_triadBuffer + m_triadCount,
		Test::IsTriadInvalid);
	m_triadCount = (int32) (lastTriad - m_triadBuffer);

	// update groups
	for (b2ParticleGroup* group = m_groupList; group; group = group->GetNext())
	{
		int32 firstIndex = newCount;
		int32 lastIndex = 0;
		bool modified = false;
		for (int32 i = group->m_firstIndex; i < group->m_lastIndex; i++)
		{
			int32 j = newIndices[i];
			if (j >= 0) {
				firstIndex = b2Min(firstIndex, j);
				lastIndex = b2Max(lastIndex, j + 1);
			} else {
				modified = true;
			}
		}
		if (firstIndex < lastIndex)
		{
			group->m_firstIndex = firstIndex;
			group->m_lastIndex = lastIndex;
			if (modified)
			{
				if (group->m_groupFlags & b2_solidParticleGroup)
				{
					SetParticleGroupFlags(group,
										  group->m_groupFlags |
											b2_particleGroupNeedsUpdateDepth);
				}
				// TODO: flag to split if needed
			}
		}
		else
		{
			group->m_firstIndex = 0;
			group->m_lastIndex = 0;
			if (!(group->m_groupFlags & b2_particleGroupCanBeEmpty))
			{
				SetParticleGroupFlags(group,
					group->m_groupFlags | b2_particleGroupWillBeDestroyed);
			}
		}
	}

	// update particle count
	m_count = newCount;
	m_world->m_stackAllocator.Free(newIndices);
	m_allParticleFlags = allParticleFlags;
	m_needsUpdateAllParticleFlags = false;

	// destroy bodies with no particles
	for (b2ParticleGroup* group = m_groupList; group;)
	{
		b2ParticleGroup* next = group->GetNext();
		if (group->m_groupFlags & b2_particleGroupWillBeDestroyed)
		{
			DestroyParticleGroup(group);
		}
		// TODO: split the group if flagged
		group = next;
	}
}

void b2ParticleSystem::RotateBuffer(int32 start, int32 mid, int32 end)
{
	// move the particles assigned to the given group toward the end of array
	if (start == mid || mid == end)
	{
		return;
	}
	struct NewIndices
	{
		int32 operator[](int32 i) const
		{
			if (i < start)
			{
				return i;
			}
			else if (i < mid)
			{
				return i + end - mid;
			}
			else if (i < end)
			{
				return i + start - mid;
			}
			else
			{
				return i;
			}
		}
		int32 start, mid, end;
	} newIndices;
	newIndices.start = start;
	newIndices.mid = mid;
	newIndices.end = end;

	std::rotate(m_flagsBuffer.data + start, m_flagsBuffer.data + mid,
				m_flagsBuffer.data + end);
	std::rotate(m_positionBuffer.data + start, m_positionBuffer.data + mid,
				m_positionBuffer.data + end);
	std::rotate(m_velocityBuffer.data + start, m_velocityBuffer.data + mid,
				m_velocityBuffer.data + end);
	std::rotate(m_groupBuffer + start, m_groupBuffer + mid,
				m_groupBuffer + end);
	if (m_staticPressureBuffer)
	{
		std::rotate(m_staticPressureBuffer + start,
					m_staticPressureBuffer + mid,
					m_staticPressureBuffer + end);
	}
	if (m_depthBuffer)
	{
		std::rotate(m_depthBuffer + start, m_depthBuffer + mid,
					m_depthBuffer + end);
	}
	if (m_colorBuffer.data)
	{
		std::rotate(m_colorBuffer.data + start,
					m_colorBuffer.data + mid, m_colorBuffer.data + end);
	}
	if (m_userDataBuffer.data)
	{
		std::rotate(m_userDataBuffer.data + start,
					m_userDataBuffer.data + mid, m_userDataBuffer.data + end);
	}

	// update proxies
	for (int32 k = 0; k < m_proxyCount; k++)
	{
		Proxy& proxy = m_proxyBuffer[k];
		proxy.index = newIndices[proxy.index];
	}

	// update contacts
	for (int32 k = 0; k < m_contactCount; k++)
	{
		b2ParticleContact& contact = m_contactBuffer[k];
		contact.indexA = newIndices[contact.indexA];
		contact.indexB = newIndices[contact.indexB];
	}

	// update particle-body contacts
	for (int32 k = 0; k < m_bodyContactCount; k++)
	{
		b2ParticleBodyContact& contact = m_bodyContactBuffer[k];
		contact.index = newIndices[contact.index];
	}

	// update pairs
	for (int32 k = 0; k < m_pairCount; k++)
	{
		Pair& pair = m_pairBuffer[k];
		pair.indexA = newIndices[pair.indexA];
		pair.indexB = newIndices[pair.indexB];
	}

	// update triads
	for (int32 k = 0; k < m_triadCount; k++)
	{
		Triad& triad = m_triadBuffer[k];
		triad.indexA = newIndices[triad.indexA];
		triad.indexB = newIndices[triad.indexB];
		triad.indexC = newIndices[triad.indexC];
	}

	// update groups
	for (b2ParticleGroup* group = m_groupList; group; group = group->GetNext())
	{
		group->m_firstIndex = newIndices[group->m_firstIndex];
		group->m_lastIndex = newIndices[group->m_lastIndex - 1] + 1;
	}
}

void b2ParticleSystem::SetStrictContactCheck(bool enabled)
{
	m_strictContactCheck = enabled;
}

bool b2ParticleSystem::GetStrictContactCheck() const
{
	return m_strictContactCheck;
}

void b2ParticleSystem::SetParticleRadius(float32 radius)
{
	m_particleDiameter = 2 * radius;
	m_squaredDiameter = m_particleDiameter * m_particleDiameter;
	m_inverseDiameter = 1 / m_particleDiameter;
}

void b2ParticleSystem::SetParticleDensity(float32 density)
{
	m_density = density;
	m_inverseDensity =  1 / m_density;
}

float32 b2ParticleSystem::GetParticleDensity() const
{
	return m_density;
}

void b2ParticleSystem::SetParticleGravityScale(float32 gravityScale)
{
	m_gravityScale = gravityScale;
}

float32 b2ParticleSystem::GetParticleGravityScale() const
{
	return m_gravityScale;
}

void b2ParticleSystem::SetParticleDamping(float32 damping)
{
	m_def.dampingStrength = damping;
}

float32 b2ParticleSystem::GetParticleDamping() const
{
	return m_def.dampingStrength;
}

void b2ParticleSystem::SetParticleStaticPressureIterations(int32 iterations)
{
	m_def.staticPressureIterations = iterations;
}

int32 b2ParticleSystem::GetParticleStaticPressureIterations() const
{
	return m_def.staticPressureIterations;
}

float32 b2ParticleSystem::GetParticleRadius() const
{
	return m_particleDiameter / 2;
}

float32 b2ParticleSystem::GetCriticalVelocity(const b2TimeStep& step) const
{
	return m_particleDiameter * step.inv_dt;
}

float32 b2ParticleSystem::GetCriticalVelocitySquared(
	const b2TimeStep& step) const
{
	float32 velocity = GetCriticalVelocity(step);
	return velocity * velocity;
}

float32 b2ParticleSystem::GetCriticalPressure(const b2TimeStep& step) const
{
	return m_density * GetCriticalVelocitySquared(step);
}

float32 b2ParticleSystem::GetParticleStride() const
{
	return b2_particleStride * m_particleDiameter;
}

float32 b2ParticleSystem::GetParticleMass() const
{
	float32 stride = GetParticleStride();
	return m_density * stride * stride;
}

float32 b2ParticleSystem::GetParticleInvMass() const
{
	return 1.777777f * m_inverseDensity * m_inverseDiameter *
			 m_inverseDiameter;
}

b2Vec2* b2ParticleSystem::GetParticlePositionBuffer()
{
	return m_positionBuffer.data;
}

b2Vec2* b2ParticleSystem::GetParticleVelocityBuffer()
{
	return m_velocityBuffer.data;
}

b2ParticleColor* b2ParticleSystem::GetParticleColorBuffer()
{
	m_colorBuffer.data = RequestParticleBuffer(m_colorBuffer.data);
	return m_colorBuffer.data;
}

void** b2ParticleSystem::GetParticleUserDataBuffer()
{
	m_userDataBuffer.data = RequestParticleBuffer(m_userDataBuffer.data);
	return m_userDataBuffer.data;
}

int32 b2ParticleSystem::GetParticleMaxCount() const
{
	return m_maxCount;
}

void b2ParticleSystem::SetParticleMaxCount(int32 count)
{
	b2Assert(m_count <= count);
	m_maxCount = count;
}

const uint32* b2ParticleSystem::GetParticleFlagsBuffer() const
{
	return m_flagsBuffer.data;
}

const b2Vec2* b2ParticleSystem::GetParticlePositionBuffer() const
{
	return m_positionBuffer.data;
}

const b2Vec2* b2ParticleSystem::GetParticleVelocityBuffer() const
{
	return m_velocityBuffer.data;
}

const b2ParticleColor* b2ParticleSystem::GetParticleColorBuffer() const
{
	return ((b2ParticleSystem*) this)->GetParticleColorBuffer();
}

const b2ParticleGroup* const* b2ParticleSystem::GetParticleGroupBuffer() const
{
	return m_groupBuffer;
}

void* const* b2ParticleSystem::GetParticleUserDataBuffer() const
{
	return ((b2ParticleSystem*) this)->GetParticleUserDataBuffer();
}

template <typename T> void b2ParticleSystem::SetParticleBuffer(
	ParticleBuffer<T>* buffer, T* newData, int32 newCapacity)
{
	b2Assert((newData && newCapacity) || (!newData && !newCapacity));
	if (!buffer->userSuppliedCapacity)
	{
		m_world->m_blockAllocator.Free(
			buffer->data, sizeof(T) * m_internalAllocatedCapacity);
	}
	buffer->data = newData;
	buffer->userSuppliedCapacity = newCapacity;
}

void b2ParticleSystem::SetParticleFlagsBuffer(uint32* buffer, int32 capacity)
{
	SetParticleBuffer(&m_flagsBuffer, buffer, capacity);
}

void b2ParticleSystem::SetParticlePositionBuffer(b2Vec2* buffer,
												 int32 capacity)
{
	SetParticleBuffer(&m_positionBuffer, buffer, capacity);
}

void b2ParticleSystem::SetParticleVelocityBuffer(b2Vec2* buffer,
												 int32 capacity)
{
	SetParticleBuffer(&m_velocityBuffer, buffer, capacity);
}

void b2ParticleSystem::SetParticleColorBuffer(b2ParticleColor* buffer,
											  int32 capacity)
{
	SetParticleBuffer(&m_colorBuffer, buffer, capacity);
}

b2ParticleGroup* const* b2ParticleSystem::GetParticleGroupBuffer()
{
	return m_groupBuffer;
}

void b2ParticleSystem::SetParticleUserDataBuffer(void** buffer, int32 capacity)
{
	SetParticleBuffer(&m_userDataBuffer, buffer, capacity);
}

void b2ParticleSystem::SetParticleFlags(int32 index, uint32 newFlags)
{
	uint32* oldFlags = &m_flagsBuffer.data[index];
	if (*oldFlags & ~newFlags)
	{
		// If any flags might be removed
		m_needsUpdateAllParticleFlags = true;
	}
	if (~m_allParticleFlags & newFlags)
	{
		// If any flags were added
		if (newFlags & b2_tensileParticle)
		{
			m_accumulation2Buffer = RequestParticleBuffer(
				m_accumulation2Buffer);
		}
		if (newFlags & b2_colorMixingParticle)
		{
			m_colorBuffer.data = RequestParticleBuffer(m_colorBuffer.data);
		}
		m_allParticleFlags |= newFlags;
	}
	*oldFlags = newFlags;
}

void b2ParticleSystem::SetParticleGroupFlags(
	b2ParticleGroup* group, uint32 newFlags)
{
	uint32* oldFlags = &group->m_groupFlags;
	if ((*oldFlags ^ newFlags) & b2_solidParticleGroup)
	{
		// If the b2_solidParticleGroup flag changed schedule depth update.
		newFlags |= b2_particleGroupNeedsUpdateDepth;
	}
	if (*oldFlags & ~newFlags)
	{
		// If any flags might be removed
		m_needsUpdateAllGroupFlags = true;
	}
	if (~m_allGroupFlags & newFlags)
	{
		// If any flags were added
		if (newFlags & b2_solidParticleGroup)
		{
			m_depthBuffer = RequestParticleBuffer(m_depthBuffer);
		}
		m_allGroupFlags |= newFlags;
	}
	*oldFlags = newFlags;
}

void b2ParticleSystem::QueryAABB(b2QueryCallback* callback,
								 const b2AABB& aabb) const
{
	if (m_proxyCount == 0)
	{
		return;
	}
	Proxy* beginProxy = m_proxyBuffer;
	Proxy* endProxy = beginProxy + m_proxyCount;
	Proxy* firstProxy = std::lower_bound(
		beginProxy, endProxy,
		computeTag(
			m_inverseDiameter * aabb.lowerBound.x,
			m_inverseDiameter * aabb.lowerBound.y));
	Proxy* lastProxy = std::upper_bound(
		firstProxy, endProxy,
		computeTag(
			m_inverseDiameter * aabb.upperBound.x,
			m_inverseDiameter * aabb.upperBound.y));
	for (Proxy* proxy = firstProxy; proxy < lastProxy; ++proxy)
	{
		int32 i = proxy->index;
		const b2Vec2& p = m_positionBuffer.data[i];
		if (aabb.lowerBound.x < p.x && p.x < aabb.upperBound.x &&
			aabb.lowerBound.y < p.y && p.y < aabb.upperBound.y)
		{
			if (!callback->ReportParticle(i))
			{
				break;
			}
		}
	}
}

void b2ParticleSystem::QueryShapeAABB(b2QueryCallback* callback,
									  const b2Shape& shape,
									  const b2Transform& xf) const
{
	b2AABB aabb;
	shape.ComputeAABB(&aabb, xf, 0);
	QueryAABB(callback, aabb);
}

void b2ParticleSystem::RayCast(b2RayCastCallback* callback,
							   const b2Vec2& point1,
							   const b2Vec2& point2) const
{
	if (m_proxyCount == 0)
	{
		return;
	}
	Proxy* beginProxy = m_proxyBuffer;
	Proxy* endProxy = beginProxy + m_proxyCount;
	Proxy* firstProxy = std::lower_bound(
		beginProxy, endProxy,
		computeTag(
			m_inverseDiameter * b2Min(point1.x, point2.x) - 1,
			m_inverseDiameter * b2Min(point1.y, point2.y) - 1));
	Proxy* lastProxy = std::upper_bound(
		firstProxy, endProxy,
		computeTag(
			m_inverseDiameter * b2Max(point1.x, point2.x) + 1,
			m_inverseDiameter * b2Max(point1.y, point2.y) + 1));
	float32 fraction = 1;
	// solving the following equation:
	// ((1-t)*point1+t*point2-position)^2=diameter^2
	// where t is a potential fraction
	b2Vec2 v = point2 - point1;
	float32 v2 = b2Dot(v, v);
	for (Proxy* proxy = firstProxy; proxy < lastProxy; ++proxy)
	{
		int32 i = proxy->index;
		b2Vec2 p = point1 - m_positionBuffer.data[i];
		float32 pv = b2Dot(p, v);
		float32 p2 = b2Dot(p, p);
		float32 determinant = pv * pv - v2 * (p2 - m_squaredDiameter);
		if (determinant >= 0)
		{
			float32 sqrtDeterminant = b2Sqrt(determinant);
			// find a solution between 0 and fraction
			float32 t = (-pv - sqrtDeterminant) / v2;
			if (t > fraction)
			{
				continue;
			}
			if (t < 0)
			{
				t = (-pv + sqrtDeterminant) / v2;
				if (t < 0 || t > fraction)
				{
					continue;
				}
			}
			b2Vec2 n = p + t * v;
			n.Normalize();
			float32 f = callback->ReportParticle(i, point1 + t * v, n, t);
			fraction = b2Min(fraction, f);
			if (fraction <= 0)
			{
				break;
			}
		}
	}
}

float32 b2ParticleSystem::ComputeParticleCollisionEnergy() const
{
	float32 sum_v2 = 0;
	for (int32 k = 0; k < m_contactCount; k++)
	{
		const b2ParticleContact& contact = m_contactBuffer[k];
		int32 a = contact.indexA;
		int32 b = contact.indexB;
		b2Vec2 n = contact.normal;
		b2Vec2 v = m_velocityBuffer.data[b] - m_velocityBuffer.data[a];
		float32 vn = b2Dot(v, n);
		if (vn < 0)
		{
			sum_v2 += vn * vn;
		}
	}
	return 0.5f * GetParticleMass() * sum_v2;
}
