
#include "BlockQueue.h"
#include <thread>
#include <sstream>
#include <libdevcore/easylog.h>
#include <libcvcore/Exceptions.h>
#include <libcvcore/BlockHeader.h>
#include "BlockChain.h"
#include "VerifiedBlock.h"
#include "State.h"
using namespace std;
using namespace dev;
using namespace dev::cv;



size_t const c_maxKnownCount = 100000;
size_t const c_maxKnownSize = 128 * 1024 * 1024;
size_t const c_maxUnknownCount = 100000;
size_t const c_maxUnknownSize = 512 * 1024 * 1024; // Block size can be ~50kb

BlockQueue::BlockQueue()
{
	// Allow some room for other activity
	unsigned verifierThreads = std::max(thread::hardware_concurrency(), 3U) - 2U;
	for (unsigned i = 0; i < verifierThreads; ++i)
		m_verifiers.emplace_back([ = ]() {
		pthread_setThreadName("verifier" + toString(i));
		this->verifierBody();
	});
}

BlockQueue::~BlockQueue()
{
	stop();
}

void BlockQueue::stop()
{
	DEV_GUARDED(m_verification)
	m_deleting = true;

	m_moreToVerify.notify_all();
	for (auto& i : m_verifiers)
		i.join();
	m_verifiers.clear();
}

void BlockQueue::clear()
{
	WriteGuard l(m_lock);
	DEV_INVARIANT_CHECK;
	Guard l2(m_verification);
	m_readySet.clear();
	m_drainingSet.clear();
	m_verified.clear();
	m_unverified.clear();
	m_verifying.clear();
	m_unknownSet.clear();
	m_unknown.clear();
	m_future.clear();
	m_difficulty = 0;
	m_drainingDifficulty = 0;
}

void BlockQueue::verifierBody()
{
	while (!m_deleting)
	{
		UnverifiedBlock work;

		{
			unique_lock<Mutex> l(m_verification);

			LOG(TRACE) << "BlockQueue::verifierBody";

			m_moreToVerify.wait(l, [&]() { return !m_unverified.isEmpty() || m_deleting; });

			if (m_deleting)
				return;

			work = m_unverified.dequeue();

			BlockHeader bi;
			bi.setSha3Uncles(work.hash);
			bi.setParentHash(work.parentHash);
			m_verifying.enqueue(move(bi));
		}

		VerifiedBlock res;
		swap(work.blockData, res.blockData);
		try
		{
			res.verified = m_bc->verifyBlock(&res.blockData, m_onBad, ImportRequirements::OutOfOrderChecks);
		}
		catch (std::exception const& _ex)
		{
			// bad block.
			// has to be this order as that's how invariants() assumes.
			LOG(WARNING) << "Unexpected exception when verifying block: " << res.verified.info.number() << _ex.what();
			WriteGuard l2(m_lock);
			unique_lock<Mutex> l(m_verification);
			m_readySet.erase(work.hash);
			m_knownBad.insert(work.hash);
			if (!m_verifying.remove(work.hash))
				LOG(WARNING) << "Unexpected exception when verifying block: " << _ex.what();
			drainVerified_WITH_BOTH_LOCKS();
			continue;
		}

		bool ready = false;
		{
			WriteGuard l2(m_lock);
			unique_lock<Mutex> l(m_verification);
			if (!m_verifying.isEmpty() && m_verifying.nextHash() == work.hash)
			{
				// we're next!
				m_verifying.dequeue();
				if (m_knownBad.count(res.verified.info.parentHash()))
				{
					m_readySet.erase(res.verified.info.hash());
					m_knownBad.insert(res.verified.info.hash());
				}
				else
					m_verified.enqueue(move(res));

				drainVerified_WITH_BOTH_LOCKS();
				ready = true;
			}
			else
			{
				if (!m_verifying.replace(work.hash, move(res)))
					LOG(WARNING) << "BlockQueue missing our job: was there a GM?";
			}
		}
		if (ready)
			m_onReady();
	}
}

void BlockQueue::drainVerified_WITH_BOTH_LOCKS()
{
	while (!m_verifying.isEmpty() && !m_verifying.next().blockData.empty())
	{
		VerifiedBlock block = m_verifying.dequeue();
		if (m_knownBad.count(block.verified.info.parentHash()))
		{
			m_readySet.erase(block.verified.info.hash());
			m_knownBad.insert(block.verified.info.hash());
		}
		else
			m_verified.enqueue(move(block));
	}
}

// 到这里之前已经经过共识（块、交易都被验证过一次），这里只要验证queue层面的合法性就够了，后续上链还会有再一次验证
ImportResult BlockQueue::importVerified(bytesConstRef _block) {
	LOG(TRACE) << std::this_thread::get_id();
	// Check if we already know this block.
	h256 h = BlockHeader::headerHashFromBlock(_block);

	LOG(TRACE) << "Queuing block" << h << "for import...";

	UpgradableGuard l(m_lock);

	if (m_readySet.count(h) || m_drainingSet.count(h) || m_unknownSet.count(h) || m_knownBad.count(h))	{
		// Already know about this one.
		LOG(WARNING) << "Already known. hash=" << h;
		return ImportResult::AlreadyKnown;
	}

	VerifiedBlock res;
	BlockHeader bi;
	try	{
		// 通过verifyBody把交易
		res.verified = m_bc->verifyBlock(_block, m_onBad, ImportRequirements::OutOfOrderChecks);
		bi = res.verified.info;
		res.blockData = _block.toVector(); // 存放真实的块数据
		res.verified.block = &res.blockData;
	}	catch (Exception const& _e)	{
		LOG(WARNING) << "Ignoring malformed block: " << diagnostic_information(_e);
		return ImportResult::Malformed;
	}

	LOG(TRACE) << "Block" << h << "is" << bi.number() << "parent is" << bi.parentHash();

	// Check block doesn't already exist first!
	if (m_bc->isKnown(h))	{
		LOG(WARNING) << "Already known in chain. hash=" << h;
		return ImportResult::AlreadyInChain;
	}

	UpgradeGuard ul(l);
	DEV_INVARIANT_CHECK;

	// 检查parent
	if (m_knownBad.count(bi.parentHash()))	{ // bad block处理
		m_knownBad.insert(bi.hash());
		updateBad_WITH_LOCK(bi.hash());
		LOG(WARNING) << "knowBad. hash=" << h;
		return ImportResult::BadChain;
	} else if (!m_bc->isKnown(bi.parentHash()))	{  // 父块必须在链上了，联盟链场景不存在叔块
		LOG(WARNING) << "unknown parent:" << bi.parentHash() << ",hash=" << h;
		m_unknown.insert(bi.parentHash(), h, _block.toBytes());
		m_unknownSet.insert(h);
		return ImportResult::UnknownParent;
	} else {
		// 本应该在check一遍 OutOfOrder，但是在blockchain.sync的时候会进行这一步，所以这里就不做了，只要不冲突就塞进去
		// 如果要做，上面的verifyBlock调用，改成OutOfOrder
		DEV_GUARDED(m_verification) {
			if (m_verified.exist(bi.hash())) {
				LOG(WARNING) << "in verified. hash=" << h;
				return ImportResult::AlreadyKnown;
			}

			LOG(TRACE) << "OK - ready for chain insertion.";
			m_verified.enqueue(move(res));
			m_difficulty += bi.difficulty();
			drainVerified_WITH_BOTH_LOCKS();
			m_onReady();
		}
	}

	return ImportResult::Success;
}

ImportResult BlockQueue::import(bytesConstRef _block, bool _isOurs)
{
	LOG(TRACE) << std::this_thread::get_id();
	// Check if we already know this block.
	h256 h = BlockHeader::headerHashFromBlock(_block);

	LOG(TRACE) << "Queuing block" << h << "for import...";

	UpgradableGuard l(m_lock);

	if (m_readySet.count(h) || m_drainingSet.count(h) || m_unknownSet.count(h) || m_knownBad.count(h))
	{
		// Already know about this one.
		LOG(TRACE) << "Already known.";
		return ImportResult::AlreadyKnown;
	}

	BlockHeader bi;
	try
	{
		// TODO: quick verification of seal - will require BlockQueue to be templated on SealEngine
		// VERIFY: populates from the block and checks the block is internally coherent.
		bi = m_bc->verifyBlock(_block, m_onBad, ImportRequirements::PostGenesis).info;
	}
	catch (Exception const& _e)
	{
		LOG(WARNING) << "Ignoring malformed block: " << diagnostic_information(_e);
		return ImportResult::Malformed;
	}

	LOG(TRACE) << "Block" << h << "is" << bi.number() << "parent is" << bi.parentHash();

	// Check block doesn't already exist first!
	if (m_bc->isKnown(h))
	{
		LOG(INFO) << "Already known in chain.";
		return ImportResult::AlreadyInChain;
	}

	UpgradeGuard ul(l);
	DEV_INVARIANT_CHECK;

	// Check it's not in the future
	if (!(m_bc->chainParams().otherParams.count("allowFutureBlocks")) && bi.timestamp() > utcTime() && !_isOurs)
	{
		m_future.insert(static_cast<uint64_t>(bi.timestamp()), h, _block.toBytes());
		char buf[24];
		time_t bit = static_cast<time_t>(bi.timestamp());
		if (strftime(buf, 24, "%X", localtime(&bit)) == 0)
			buf[0] = '\0'; // empty if case strftime fails
		LOG(TRACE) << "OK - queued for future [" << bi.timestamp() << "vs" << utcTime() << "] - will wait until" << buf;
		m_difficulty += bi.difficulty();
		bool unknown =  !m_readySet.count(bi.parentHash()) && !m_drainingSet.count(bi.parentHash()) && !m_bc->isKnown(bi.parentHash());
		return unknown ? ImportResult::FutureTimeUnknown : ImportResult::FutureTimeKnown;
	}
	else
	{
		// We now know it.
		if (m_knownBad.count(bi.parentHash()))
		{
			m_knownBad.insert(bi.hash());
			updateBad_WITH_LOCK(bi.hash());
			// bad parent; this is bad too, note it as such
			return ImportResult::BadChain;
		}
		else if (!m_readySet.count(bi.parentHash()) && !m_drainingSet.count(bi.parentHash()) && !m_bc->isKnown(bi.parentHash()))
		{
			// We don't know the parent (yet) - queue it up for later. It'll get resent to us if we find out about its ancestry later on.
			LOG(TRACE) << "OK - queued as unknown parent:" << bi.parentHash();
			m_unknown.insert(bi.parentHash(), h, _block.toBytes());
			m_unknownSet.insert(h);
			m_difficulty += bi.difficulty();

			return ImportResult::UnknownParent;
		}
		else
		{
			// If valid, append to blocks.
			LOG(TRACE) << "OK - ready for chain insertion.";
			DEV_GUARDED(m_verification)
			m_unverified.enqueue(UnverifiedBlock { h, bi.parentHash(), _block.toBytes() });
			m_moreToVerify.notify_one();
			m_readySet.insert(h);
			m_difficulty += bi.difficulty();

			noteReady_WITH_LOCK(h);

			return ImportResult::Success;
		}
	}
}

void BlockQueue::updateBad_WITH_LOCK(h256 const& _bad)
{
	DEV_INVARIANT_CHECK;
	DEV_GUARDED(m_verification)
	{
		collectUnknownBad_WITH_BOTH_LOCKS(_bad);
		bool moreBad = true;
		while (moreBad)
		{
			moreBad = false;
			std::vector<VerifiedBlock> badVerified = m_verified.removeIf([this](VerifiedBlock const & _b)
			{
				return m_knownBad.count(_b.verified.info.parentHash()) || m_knownBad.count(_b.verified.info.hash());
			});
			for (auto& b : badVerified)
			{
				m_knownBad.insert(b.verified.info.hash());
				m_readySet.erase(b.verified.info.hash());
				collectUnknownBad_WITH_BOTH_LOCKS(b.verified.info.hash());
				moreBad = true;
			}

			std::vector<UnverifiedBlock> badUnverified = m_unverified.removeIf([this](UnverifiedBlock const & _b)
			{
				return m_knownBad.count(_b.parentHash) || m_knownBad.count(_b.hash);
			});
			for (auto& b : badUnverified)
			{
				m_knownBad.insert(b.hash);
				m_readySet.erase(b.hash);
				collectUnknownBad_WITH_BOTH_LOCKS(b.hash);
				moreBad = true;
			}

			std::vector<VerifiedBlock> badVerifying = m_verifying.removeIf([this](VerifiedBlock const & _b)
			{
				return m_knownBad.count(_b.verified.info.parentHash()) || m_knownBad.count(_b.verified.info.sha3Uncles());
			});
			for (auto& b : badVerifying)
			{
				h256 const& h = b.blockData.size() != 0 ? b.verified.info.hash() : b.verified.info.sha3Uncles();
				m_knownBad.insert(h);
				m_readySet.erase(h);
				collectUnknownBad_WITH_BOTH_LOCKS(h);
				moreBad = true;
			}
		}
	}
}

void BlockQueue::collectUnknownBad_WITH_BOTH_LOCKS(h256 const& _bad)
{
	list<h256> badQueue(1, _bad);
	while (!badQueue.empty())
	{
		vector<pair<h256, bytes>> const removed = m_unknown.removeByKeyEqual(badQueue.front());
		badQueue.pop_front();
		for (auto& newBad : removed)
		{
			m_unknownSet.erase(newBad.first);
			m_knownBad.insert(newBad.first);
			badQueue.push_back(newBad.first);
		}
	}
}

bool BlockQueue::doneDrain(h256s const& _bad)
{
	WriteGuard l(m_lock);
	DEV_INVARIANT_CHECK;
	m_drainingSet.clear();
	m_difficulty -= m_drainingDifficulty;
	m_drainingDifficulty = 0;
	if (_bad.size())
	{
		// at least one of them was bad.
		m_knownBad += _bad;
		for (h256 const& b : _bad)
			updateBad_WITH_LOCK(b);
	}
	return !m_readySet.empty();
}

void BlockQueue::tick()
{
	vector<pair<h256, bytes>> todo;
	{
		UpgradableGuard l(m_lock);
		if (m_future.isEmpty())
			return;

		LOG(INFO) << "Checking past-future blocks...";

		uint64_t t = utcTime();
		if (t < m_future.firstKey())
			return;

		LOG(INFO) << "Past-future blocks ready.";

		{
			UpgradeGuard l2(l);
			DEV_INVARIANT_CHECK;
			todo = m_future.removeByKeyNotGreater(t);
		}
	}
	LOG(INFO) << "Importing" << todo.size() << "past-future blocks.";

	for (auto const& b : todo) {
		LOG(INFO) << "Importing past-future block, hash=" << b.first;
		import(&b.second);
	}
}

BlockQueueStatus BlockQueue::status() const
{
	ReadGuard l(m_lock);
	Guard l2(m_verification);
	return BlockQueueStatus{ m_drainingSet.size(), m_verified.count(), m_verifying.count(), m_unverified.count(),
	                         m_future.count(), m_unknown.count(), m_knownBad.size() };
}

QueueStatus BlockQueue::blockStatus(h256 const& _h) const
{
	ReadGuard l(m_lock);
	return
	    m_readySet.count(_h) ?
	    QueueStatus::Ready :
	    m_drainingSet.count(_h) ?
	    QueueStatus::Importing :
	    m_unknownSet.count(_h) ?
	    QueueStatus::UnknownParent :
	    m_knownBad.count(_h) ?
	    QueueStatus::Bad :
	    QueueStatus::Unknown;
}

bool BlockQueue::knownFull() const
{
	Guard l(m_verification);
	return knownSize() > c_maxKnownSize || knownCount() > c_maxKnownCount;
}

std::size_t BlockQueue::knownSize() const
{
	return m_verified.size() + m_unverified.size() + m_verifying.size();
}

std::size_t BlockQueue::knownCount() const
{
	return m_verified.count() + m_unverified.count() + m_verifying.count();
}

bool BlockQueue::unknownFull() const
{
	ReadGuard l(m_lock);
	return unknownSize() > c_maxUnknownSize || unknownCount() > c_maxUnknownCount;
}

std::size_t BlockQueue::unknownSize() const
{
	return m_future.size() + m_unknown.size();
}

std::size_t BlockQueue::unknownCount() const
{
	return m_future.count() + m_unknown.count();
}

void BlockQueue::drain(VerifiedBlocks& o_out, unsigned _max)
{
	bool wasFull = false;
	DEV_WRITE_GUARDED(m_lock)
	{
		DEV_INVARIANT_CHECK;
		wasFull = knownFull();
		if (m_drainingSet.empty())
		{
			m_drainingDifficulty = 0;
			DEV_GUARDED(m_verification)
			o_out = m_verified.dequeueMultiple(min<unsigned>(_max, m_verified.count()));

			for (auto const& bs : o_out)
			{
				// TODO: @optimise use map<h256, bytes> rather than vector<bytes> & set<h256>.
				auto h = bs.verified.info.hash();
				m_drainingSet.insert(h);
				m_drainingDifficulty += bs.verified.info.difficulty();
				m_readySet.erase(h);
			}
		}
	}
	if (wasFull && !knownFull())
		m_onRoomAvailable();
}

bool BlockQueue::invariants() const
{
	Guard l(m_verification);
	if (m_readySet.size() != knownCount())
	{
		std::stringstream s;
		s << "Failed BlockQueue invariant: m_readySet: " << m_readySet.size() << " m_verified: " << m_verified.count() << " m_unverified: " << m_unverified.count() << " m_verifying" << m_verifying.count();
		BOOST_THROW_EXCEPTION(FailedInvariant() << errinfo_comment(s.str()));
	}
	return true;
}

void BlockQueue::noteReady_WITH_LOCK(h256 const& _good)
{
	DEV_INVARIANT_CHECK;
	list<h256> goodQueue(1, _good);
	bool notify = false;
	while (!goodQueue.empty())
	{
		h256 const parent = goodQueue.front();
		vector<pair<h256, bytes>> const removed = m_unknown.removeByKeyEqual(parent);
		goodQueue.pop_front();
		for (auto& newReady : removed)
		{
			DEV_GUARDED(m_verification)
			m_unverified.enqueue(UnverifiedBlock { newReady.first, parent, move(newReady.second) });
			m_unknownSet.erase(newReady.first);
			m_readySet.insert(newReady.first);
			goodQueue.push_back(newReady.first);
			notify = true;
		}
	}
	if (notify)
		m_moreToVerify.notify_all();
}

void BlockQueue::retryAllUnknown()
{
	WriteGuard l(m_lock);
	DEV_INVARIANT_CHECK;
	while (!m_unknown.isEmpty())
	{
		h256 parent = m_unknown.firstKey();
		vector<pair<h256, bytes>> removed = m_unknown.removeByKeyEqual(parent);
		for (auto& newReady : removed)
		{
			DEV_GUARDED(m_verification)
			m_unverified.enqueue(UnverifiedBlock{ newReady.first, parent, move(newReady.second) });
			m_unknownSet.erase(newReady.first);
			m_readySet.insert(newReady.first);
			m_moreToVerify.notify_one();
		}
	}
	m_moreToVerify.notify_all();
}

std::ostream& dev::eth::operator<<(std::ostream& _out, BlockQueueStatus const& _bqs)
{
	_out << "importing: " << _bqs.importing << "\n";
	_out << "verified: " << _bqs.verified << "\n";
	_out << "verifying: " << _bqs.verifying << "\n";
	_out << "unverified: " << _bqs.unverified << "\n";
	_out << "future: " << _bqs.future << "\n";
	_out << "unknown: " << _bqs.unknown << "\n";
	_out << "bad: " << _bqs.bad << "\n";

	return _out;
}

u256 BlockQueue::difficulty() const
{
	UpgradableGuard l(m_lock);
	return m_difficulty;
}

bool BlockQueue::isActive() const
{
	UpgradableGuard l(m_lock);
	if (m_readySet.empty() && m_drainingSet.empty())
		DEV_GUARDED(m_verification)
		if (m_verified.isEmpty() && m_verifying.isEmpty() && m_unverified.isEmpty())
			return false;
	return true;
}

std::ostream& dev::eth::operator<< (std::ostream& os, QueueStatus const& obj)
{
	os << static_cast<std::underlying_type<QueueStatus>::type>(obj);
	return os;
}
