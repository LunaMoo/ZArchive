#include "zarchive/zarchivewriter.h"
#include "zarchive/zarchivecommon.h"

#include <string>
#include <string_view>
#include <queue>

#include <zstd.h>

#include "sha_256.h"

#include <cassert>
#include <algorithm>
#include <cstdlib>

ZArchiveWriter::ZArchiveWriter(CB_NewOutputFile cbNewOutputFile, CB_WriteOutputData cbWriteOutputData, void* ctx) : m_cbNewOutputFile(cbNewOutputFile), m_cbWriteOutputData(cbWriteOutputData), m_cbCtx(ctx)
{
	cbNewOutputFile(-1, ctx);
	m_mainShaCtx = (struct Sha_256*)malloc(sizeof(struct Sha_256));
	sha_256_init(m_mainShaCtx, m_integritySha);
};

ZArchiveWriter::~ZArchiveWriter()
{
	StopWorkers();
	free(m_mainShaCtx);
}

ZArchiveWriter::PathNode* ZArchiveWriter::GetNodeByPath(ZArchiveWriter::PathNode* root, std::string_view path)
{
	PathNode* currentNode = &m_rootNode;

	std::string_view pathParser = path;
	while (true)
	{
		std::string_view nodeName;
		if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
			break;
		PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
		if (!nextSubnode || (nextSubnode && nextSubnode->isFile))
			return nullptr;
		currentNode = nextSubnode;
	}
	return currentNode;
}

ZArchiveWriter::PathNode* ZArchiveWriter::FindSubnodeByName(ZArchiveWriter::PathNode* parent, std::string_view nodeName)
{
	for (auto& it : parent->subnodes)
	{
		std::string_view itName = m_nodeNames[it->nameIndex];
		if (_ZARCHIVE::CompareNodeNameBool(itName, nodeName))
			return it;
	}
	return nullptr;
}

bool ZArchiveWriter::StartNewFile(const char* path)
{
	return StartNewFile(path, true);
}

bool ZArchiveWriter::StartNewFile(const char* path, bool compress)
{
	m_currentFileNode = nullptr;
	m_currentFileCompress = compress;
	std::string_view pathParser = path;
	std::string_view filename;
	_ZARCHIVE::SplitFilenameFromPath(pathParser, filename);
	PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
	if (!dir)
		return false;
	if (FindSubnodeByName(dir, filename))
		return false;
	// add new entry and make it the currently active file for append operations
	PathNode*& r = dir->subnodes.emplace_back(new PathNode(true, CreateNameEntry(filename)));
	m_currentFileNode = r;
	r->fileOffset = m_currentInputOffset;
	return true;
}

bool ZArchiveWriter::MakeDir(const char* path, bool recursive)
{
	std::string_view pathParser = path;
	while (!pathParser.empty() && (pathParser.back() == '/' || pathParser.back() == '\\'))
		pathParser.remove_suffix(1);
	if (!recursive)
	{
		std::string_view dirName;
		_ZARCHIVE::SplitFilenameFromPath(pathParser, dirName);
		PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
		if (!dir)
			return false;
		if (FindSubnodeByName(dir, dirName))
			return false;
		dir->subnodes.emplace_back(new PathNode(false, CreateNameEntry(dirName)));
	}
	else
	{
		PathNode* currentNode = &m_rootNode;
		while (true)
		{
			std::string_view nodeName;
			if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
				break;
			PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
			if (nextSubnode && nextSubnode->isFile)
				return false;
			if (!nextSubnode)
			{
				PathNode*& r = currentNode->subnodes.emplace_back(new PathNode(false, CreateNameEntry(nodeName)));
				nextSubnode = r;
			}
			currentNode = nextSubnode;
		}
	}
	return true;
}

uint32_t ZArchiveWriter::CreateNameEntry(std::string_view name)
{
	auto it = m_nodeNameLookup.find(std::string(name));
	if (it != m_nodeNameLookup.end())
		return it->second;
	uint32_t nameIndex = (uint32_t)m_nodeNames.size();
	m_nodeNames.emplace_back(name);
	m_nodeNameLookup.emplace(name, nameIndex);
	return nameIndex;
}

void ZArchiveWriter::OutputData(const void* data, size_t length)
{
	m_cbWriteOutputData(data, length, m_cbCtx);
	m_currentCompressedWriteIndex += length;
	m_stats.outputBytes += length;
	// hash the data
	if (m_mainShaCtx)
		sha_256_write(m_mainShaCtx, data, length);
}

uint64_t ZArchiveWriter::GetCurrentOutputOffset() const
{
	return m_currentCompressedWriteIndex;
}

// ---- Settings ---------------------------------------------------------------------------------

void ZArchiveWriter::SetCompressionLevel(int level)
{
	if (level < ZSTD_minCLevel())
		level = ZSTD_minCLevel();
	if (level > ZSTD_maxCLevel())
		level = ZSTD_maxCLevel();
	if (level == 0)
		level = ZSTD_CLEVEL_DEFAULT; // zstd reads 0 as "default"; say which
	m_compressionLevel = level;
}

void ZArchiveWriter::SetPrecheck(double percent)
{
	if (!(percent > 0.0))
		percent = 0.0;
	if (percent > 100.0)
		percent = 100.0;
	m_precheckPercent = percent;
}

void ZArchiveWriter::SetThreads(unsigned threads)
{
	if (m_started)
		return; // too late, keep what the blocks so far were batched with
	if (threads == 0)
		threads = std::max(1u, std::thread::hardware_concurrency());
	m_threads = std::min(threads, 256u);
}

ZArchiveWriter::Stats ZArchiveWriter::GetStats() const
{
	return m_stats;
}

// ---- Block compression ------------------------------------------------------------------------

struct ZArchiveWriter::Compressor
{
	ZSTD_CCtx* cctx = ZSTD_createCCtx();
	std::vector<uint8_t> trial; // the pre-check's output
	~Compressor() { ZSTD_freeCCtx(cctx); }
};

void ZArchiveWriter::CompressBlock(Compressor& c, BlockJob& job) const
{
	const size_t blockSize = _ZARCHIVE::COMPRESSED_BLOCK_SIZE;
	job.stored = true;
	job.precheckSkipped = false;
	job.outputSize = blockSize;
	if (!job.compress)
		return;
	const size_t bound = ZSTD_compressBound(blockSize);
	if (job.output.size() < bound)
		job.output.resize(bound);
	if (m_precheckPercent > 0.0)
	{
		// The fast trial. Blocks it can't shrink enough are stored as they are.
		const int fastLevel = std::min(1, m_compressionLevel);
		size_t trialSize = ZSTD_compressCCtx(c.cctx, job.output.data(), job.output.size(), job.input.data(), blockSize, fastLevel);
		if (ZSTD_isError(trialSize) || (double)trialSize > (double)blockSize * m_precheckPercent / 100.0)
		{
			job.precheckSkipped = true;
			return;
		}
		if (m_compressionLevel <= fastLevel)
		{
			job.stored = false;
			job.outputSize = trialSize;
			return;
		}
	}
	size_t size = ZSTD_compressCCtx(c.cctx, job.output.data(), job.output.size(), job.input.data(), blockSize, m_compressionLevel);
	// store the block uncompressed if it is equal or larger than the input after compression
	if (ZSTD_isError(size) || size >= blockSize)
		return;
	job.stored = false;
	job.outputSize = size;
}

void ZArchiveWriter::WriteBlock(const uint8_t* data, size_t size)
{
	// add offset translation record
	uint64_t compressedWriteOffset = GetCurrentOutputOffset();
	OutputData(data, size);
	if ((m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD) == 0)
		m_compressionOffsetRecord.emplace_back().baseOffset = compressedWriteOffset;
	m_compressionOffsetRecord.back().size[m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD] = (uint16_t)(size - 1);
	m_numWrittenOffsetRecords++;
}

void ZArchiveWriter::QueueBlock(const uint8_t* uncompressedData, bool compress)
{
	if (!m_started)
	{
		m_started = true;
		if (m_threads > 1)
			StartWorkers();
		else
			m_mainCompressor = std::make_unique<Compressor>();
	}
	auto account = [&](const BlockJob& job) {
		m_stats.blocks++;
		if (job.stored)
		{
			m_stats.storedBlocks++;
			if (!job.compress)
				m_stats.storeOnlyBlocks++;
			if (job.precheckSkipped)
				m_stats.precheckSkipped++;
		}
	};
	if (m_threads <= 1)
	{
		BlockJob& job = m_singleJob;
		job.input.assign(uncompressedData, uncompressedData + _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
		job.compress = compress;
		CompressBlock(*m_mainCompressor, job);
		WriteBlock(job.stored ? job.input.data() : job.output.data(), job.outputSize);
		account(job);
		return;
	}
	BlockJob& job = m_batch[m_batchUsed++];
	job.input.assign(uncompressedData, uncompressedData + _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	job.compress = compress;
	if (m_batchUsed == m_batch.size())
		FlushBatch();
}

void ZArchiveWriter::FlushBatch()
{
	if (m_batchUsed == 0)
		return;
	{
		std::lock_guard<std::mutex> g(m_workMutex);
		m_nextJob = 0;
		m_jobsDone = 0;
		m_runCount = m_batchUsed;
		m_workGeneration++;
	}
	m_workCv.notify_all();
	RunBatchJobs(); // the calling thread helps
	{
		std::unique_lock<std::mutex> lock(m_workMutex);
		m_doneCv.wait(lock, [&] { return m_jobsDone >= m_runCount && m_workersBusy == 0; });
		m_runCount = 0; // a worker waking up late finds nothing to do
	}
	// write in order
	for (size_t i = 0; i < m_batchUsed; i++)
	{
		BlockJob& job = m_batch[i];
		WriteBlock(job.stored ? job.input.data() : job.output.data(), job.outputSize);
		m_stats.blocks++;
		if (job.stored)
		{
			m_stats.storedBlocks++;
			if (!job.compress)
				m_stats.storeOnlyBlocks++;
			if (job.precheckSkipped)
				m_stats.precheckSkipped++;
		}
	}
	m_batchUsed = 0;
}

void ZArchiveWriter::RunBatchJobs()
{
	thread_local std::unique_ptr<Compressor> compressor;
	if (!compressor)
		compressor = std::make_unique<Compressor>();
	size_t done = 0;
	for (;;)
	{
		size_t i = m_nextJob.fetch_add(1);
		if (i >= m_runCount.load())
			break;
		CompressBlock(*compressor, m_batch[i]);
		done++;
	}
	if (done)
	{
		std::lock_guard<std::mutex> g(m_workMutex);
		m_jobsDone += done;
	}
	m_doneCv.notify_all();
}

void ZArchiveWriter::StartWorkers()
{
	// A few blocks per thread per batch keeps every core busy without holding much memory
	// (64KiB in + ~64KiB out per block).
	m_batch.resize((size_t)m_threads * 8);
	for (unsigned i = 0; i + 1 < m_threads; i++)
		m_workers.emplace_back([this] { WorkerMain(); });
}

void ZArchiveWriter::StopWorkers()
{
	{
		std::lock_guard<std::mutex> g(m_workMutex);
		m_stopWorkers = true;
	}
	m_workCv.notify_all();
	for (std::thread& t : m_workers)
		t.join();
	m_workers.clear();
}

void ZArchiveWriter::WorkerMain()
{
	uint64_t seen = 0;
	std::unique_lock<std::mutex> lock(m_workMutex);
	for (;;)
	{
		m_workCv.wait(lock, [&] { return m_stopWorkers || m_workGeneration != seen; });
		if (m_stopWorkers)
			return;
		seen = m_workGeneration;
		m_workersBusy++;
		lock.unlock();
		RunBatchJobs();
		lock.lock();
		m_workersBusy--;
		m_doneCv.notify_all();
	}
}

// ---- Appending --------------------------------------------------------------------------------

void ZArchiveWriter::AppendData(const void* data, size_t size)
{
	size_t dataSize = size;
	const uint8_t* input = (const uint8_t*)data;
	// The padding Finalize adds has no file: it never makes a block compressible on its own.
	const bool compress = m_currentFileNode != nullptr && m_currentFileCompress;
	while (size > 0)
	{
		size_t bytesToCopy = _ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size();
		if (bytesToCopy > size)
			bytesToCopy = size;
		if (bytesToCopy == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			// if incoming data is block-aligned we can store it directly without memcpy to temporary buffer
			QueueBlock(input, compress);
			input += bytesToCopy;
			size -= bytesToCopy;
			continue;
		}
		m_currentWriteBuffer.insert(m_currentWriteBuffer.end(), input, input + bytesToCopy);
		m_currentBlockCompress |= compress;
		input += bytesToCopy;
		size -= bytesToCopy;
		if (m_currentWriteBuffer.size() == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			QueueBlock(m_currentWriteBuffer.data(), m_currentBlockCompress);
			m_currentWriteBuffer.clear();
			m_currentBlockCompress = false;
		}
	}
	if (m_currentFileNode)
	{
		m_currentFileNode->fileSize += dataSize;
		m_stats.inputBytes += dataSize;
	}
	m_currentInputOffset += dataSize;
}

bool ZArchiveWriter::AddFile(const char* path, const void* data, size_t size, bool compress)
{
	if (!StartNewFile(path, compress))
		return false;
	AppendData(data, size);
	return true;
}

void ZArchiveWriter::Finalize()
{
	m_currentFileNode = nullptr; // make sure the padding added below doesn't modify the active file
	// flush write buffer by padding it to the length of a full block
	if (!m_currentWriteBuffer.empty())
	{
		std::vector<uint8_t> padBuffer;
		padBuffer.resize(_ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size());
		AppendData(padBuffer.data(), padBuffer.size());
	}
	FlushBatch();
	StopWorkers();
	m_footer.sectionCompressedData.offset = 0;
	m_footer.sectionCompressedData.size = GetCurrentOutputOffset();
	// pad to 8 byte
	while ((GetCurrentOutputOffset() % 8) != 0)
	{
		uint8_t b = 0;
		OutputData(&b, sizeof(uint8_t));
	}
	WriteOffsetRecords();
	WriteNameTable();
	WriteFileTree();
	WriteMetaData();
	WriteFooter();
}

void ZArchiveWriter::WriteOffsetRecords()
{
	m_footer.sectionOffsetRecords.offset = GetCurrentOutputOffset();
	_ZARCHIVE::CompressionOffsetRecord::Serialize(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size(), m_compressionOffsetRecord.data()); // in-place
	OutputData(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size() * sizeof(_ZARCHIVE::CompressionOffsetRecord));
	m_footer.sectionOffsetRecords.size = GetCurrentOutputOffset() - m_footer.sectionOffsetRecords.offset;
}

void ZArchiveWriter::WriteNameTable()
{
	m_footer.sectionNames.offset = GetCurrentOutputOffset();
	uint32_t currentNameTableOffset = 0;
	m_nodeNameOffsets.resize(m_nodeNames.size());
	for (size_t i = 0; i < m_nodeNames.size(); i++)
	{
		m_nodeNameOffsets[i] = currentNameTableOffset;
		// Each node name is stored with a length prefix byte. The prefix byte's MSB is used to indicate if an extended 2-byte header is used. The lower 7 bits are used to store the lower bits of the name length
		// If MSB is set, add an extra byte which extends the 7 bit name length field to 15 bit
		std::string_view name = m_nodeNames[i];
		if (name.size() > 0x7FFF)
			name = name.substr(0, 0x7FFF); // cut-off after 2^15-1 characters
		if (name.size() >= 0x80)
		{
			uint8_t header[2];
			header[0] = (uint8_t)(name.size() & 0x7F) | 0x80;
			header[1] = (uint8_t)(name.size() >> 7);
			OutputData(header, 2);
			currentNameTableOffset += 2;
		}
		else
		{
			uint8_t header[1];
			header[0] = (uint8_t)name.size() & 0x7F;
			OutputData(header, 1);
			currentNameTableOffset += 1;
		}
		OutputData(name.data(), name.size());
		currentNameTableOffset += (uint32_t)name.size();
	}
	m_footer.sectionNames.size = GetCurrentOutputOffset() - m_footer.sectionNames.offset;
}

void ZArchiveWriter::WriteFileTree()
{
	std::queue<PathNode*> nodeQueue;
	// first pass - assign a node range to all directories
	nodeQueue.push(&m_rootNode);
	uint32_t currentIndex = 1; // root node is at index 0
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();
		if (node->isFile)
		{
			node->nodeStartIndex = (uint32_t)0xFFFFFFFF;
			continue;
		}
		// order entries lexicographically so we can use binary search in the reader
		std::sort(node->subnodes.begin(), node->subnodes.end(),
			[&](ZArchiveWriter::PathNode*& a, ZArchiveWriter::PathNode*& b) -> int
			{
				return _ZARCHIVE::CompareNodeName(m_nodeNames[a->nameIndex], m_nodeNames[b->nameIndex]) > 0;
			});

		node->nodeStartIndex = currentIndex;
		currentIndex += (uint32_t)node->subnodes.size();
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}
	// second pass - serialize to file
	m_footer.sectionFileTree.offset = GetCurrentOutputOffset();
	nodeQueue.push(&m_rootNode);
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();

		_ZARCHIVE::FileDirectoryEntry tmp;
		if(node == &m_rootNode)
			tmp.SetTypeAndNameOffset(node->isFile, 0x7FFFFFFF);
		else
			tmp.SetTypeAndNameOffset(node->isFile, m_nodeNameOffsets[node->nameIndex]);
		if (node->isFile)
		{
			tmp.SetFileOffset(node->fileOffset);
			tmp.SetFileSize(node->fileSize);
		}
		else
		{
			tmp.directoryRecord.count = (uint32_t)node->subnodes.size();
			tmp.directoryRecord.nodeStartIndex = node->nodeStartIndex;
			tmp.directoryRecord._reserved = 0;
		}
		_ZARCHIVE::FileDirectoryEntry::Serialize(&tmp, 1, &tmp);
		OutputData(&tmp, sizeof(_ZARCHIVE::FileDirectoryEntry));
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}
	m_footer.sectionFileTree.size = GetCurrentOutputOffset() - m_footer.sectionFileTree.offset;
}

void ZArchiveWriter::WriteMetaData()
{
	// todo
	m_footer.sectionMetaDirectory.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaDirectory.size = 0;
	m_footer.sectionMetaData.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaData.size = 0;
}

void ZArchiveWriter::WriteFooter()
{
	m_footer.magic = _ZARCHIVE::Footer::kMagic;
	m_footer.version = _ZARCHIVE::Footer::kVersion1;
	m_footer.totalSize = GetCurrentOutputOffset() + sizeof(_ZARCHIVE::Footer);

	_ZARCHIVE::Footer tmp;

	// serialize and hash the footer with all hash bytes set to zero
	memset(m_footer.integrityHash, 0, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	sha_256_write(m_mainShaCtx, &tmp, sizeof(_ZARCHIVE::Footer));
	sha_256_close(m_mainShaCtx);
	free(m_mainShaCtx);
	m_mainShaCtx = nullptr;

	// set hash and write footer
	memcpy(m_footer.integrityHash, m_integritySha, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	OutputData(&tmp, sizeof(_ZARCHIVE::Footer));
}
