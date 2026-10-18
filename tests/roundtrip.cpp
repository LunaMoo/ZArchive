// Writes archives from memory with every combination of the writer's settings, reads them back
// from memory and checks every byte. Exit code 0 = all good.
#include "zarchive/zarchivereader.h"
#include "zarchive/zarchivewriter.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

#define CHECK(cond, ...)                         \
	do                                           \
	{                                            \
		if (!(cond))                             \
		{                                        \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                 \
			printf("\n");                        \
			g_failures++;                        \
		}                                        \
	} while (0)

	struct TestFile
	{
		std::string path;
		std::vector<uint8_t> data;
		bool compress = true;
	};

	std::vector<uint8_t> RandomBytes(std::mt19937& rng, size_t n)
	{
		std::vector<uint8_t> v(n);
		for (auto& b : v)
			b = (uint8_t)rng();
		return v;
	}

	std::vector<uint8_t> TextBytes(std::mt19937& rng, size_t n)
	{
		static const char* words[] = { "PSP_GAME ", "USRDIR ", "EBOOT.BIN ", "PARAM.SFO ", "data ", "DLC ", "\n" };
		std::vector<uint8_t> v;
		while (v.size() < n)
		{
			const char* w = words[rng() % 7];
			v.insert(v.end(), w, w + strlen(w));
		}
		v.resize(n);
		return v;
	}

	std::vector<TestFile> MakeFiles()
	{
		std::mt19937 rng(1234);
		std::vector<TestFile> f;
		// An "already compressed disc": random data, not block aligned.
		f.push_back({ "Game.cso", RandomBytes(rng, 5 * 65536 + 777) });
		// The same kind of data marked store-only.
		f.push_back({ "Game2.chd", RandomBytes(rng, 3 * 65536 + 12), false });
		// Compressible store-only data: must still be stored as it is.
		f.push_back({ "Plain.iso", TextBytes(rng, 4 * 65536), false });
		// DLC folders with compressible and empty files.
		f.push_back({ "DLC/ULUS10000-DLC1/data.bin", TextBytes(rng, 300000) });
		f.push_back({ "DLC/ULUS10000-DLC1/empty.bin", {} });
		f.push_back({ "DLC/ULUS10000-DLC2/zeros.bin", std::vector<uint8_t>(200000, 0) });
		for (int i = 0; i < 40; i++)
			f.push_back({ "DLC/small/file" + std::to_string(i) + ".txt", TextBytes(rng, 1 + (rng() % 3000)) });
		return f;
	}

	struct Settings
	{
		int level;
		double precheck;
		unsigned threads;
	};

	std::vector<uint8_t> Write(const std::vector<TestFile>& files, const Settings& s, ZArchiveWriter::Stats* stats)
	{
		std::vector<uint8_t> out;
		ZArchiveWriter w([](int32_t, void*) {}, [](const void* d, size_t n, void* ctx) {
			auto* o = (std::vector<uint8_t>*)ctx;
			o->insert(o->end(), (const uint8_t*)d, (const uint8_t*)d + n);
		}, &out);
		w.SetCompressionLevel(s.level);
		w.SetPrecheck(s.precheck);
		w.SetThreads(s.threads);
		w.MakeDir("DLC/ULUS10000-DLC1", true);
		w.MakeDir("DLC/ULUS10000-DLC2", true);
		w.MakeDir("DLC/small", true);
		for (const TestFile& f : files)
		{
			if (f.path == "Game.cso")
			{
				// Appended in odd pieces, like a library writing its output.
				if (!w.StartNewFile(f.path.c_str(), f.compress))
					CHECK(false, "StartNewFile %s", f.path.c_str());
				size_t pos = 0, step = 1000;
				while (pos < f.data.size())
				{
					size_t n = std::min(step, f.data.size() - pos);
					w.AppendData(f.data.data() + pos, n);
					pos += n;
					step = step * 3 + 1;
				}
			}
			else
			{
				CHECK(w.AddFile(f.path.c_str(), f.data.data(), f.data.size(), f.compress), "AddFile %s", f.path.c_str());
			}
		}
		CHECK(!w.AddFile("Game.cso", "x", 1), "duplicate names must be refused");
		w.Finalize();
		*stats = w.GetStats();
		return out;
	}

	void Verify(const std::vector<TestFile>& files, const std::vector<uint8_t>& archive, const char* what)
	{
		ZArchiveReader* r = ZArchiveReader::OpenFromMemory(archive.data(), archive.size());
		CHECK(r, "%s: can't open", what);
		if (!r)
			return;
		for (const TestFile& f : files)
		{
			ZArchiveNodeHandle h = r->LookUp(f.path, true, false);
			CHECK(h != ZARCHIVE_INVALID_NODE, "%s: %s missing", what, f.path.c_str());
			if (h == ZARCHIVE_INVALID_NODE)
				continue;
			CHECK(r->GetFileSize(h) == f.data.size(), "%s: %s size", what, f.path.c_str());
			std::vector<uint8_t> back(f.data.size() + 16, 0xCC);
			uint64_t n = r->ReadFromFile(h, 0, back.size(), back.data());
			CHECK(n == f.data.size() && memcmp(back.data(), f.data.data(), f.data.size()) == 0, "%s: %s content", what, f.path.c_str());
			// a random read in the middle
			if (f.data.size() > 70000)
			{
				uint8_t b[100];
				CHECK(r->ReadFromFile(h, 65500, 100, b) == 100 && memcmp(b, f.data.data() + 65500, 100) == 0, "%s: %s random read", what, f.path.c_str());
			}
		}
		CHECK(r->LookUp("DLC/ULUS10000-DLC1", false, true) != ZARCHIVE_INVALID_NODE, "%s: folder", what);
		delete r;
		// the owning overload
		std::vector<uint8_t> copy = archive;
		r = ZArchiveReader::OpenFromMemory(std::move(copy));
		CHECK(r && r->LookUp("Plain.iso") != ZARCHIVE_INVALID_NODE, "%s: owning open", what);
		delete r;
	}
}

int main()
{
	const std::vector<TestFile> files = MakeFiles();
	uint64_t total = 0;
	for (auto& f : files)
		total += f.data.size();

	for (int level : { 1, 6, 19, 22 })
	{
		for (double precheck : { 0.0, 95.0 })
		{
			std::vector<uint8_t> first;
			for (unsigned threads : { 1u, 3u, 8u })
			{
				Settings s{ level, precheck, threads };
				ZArchiveWriter::Stats st;
				std::vector<uint8_t> a = Write(files, s, &st);
				char what[64];
				snprintf(what, sizeof(what), "level %d precheck %g threads %u", level, precheck, threads);
				Verify(files, a, what);
				CHECK(st.inputBytes == total, "%s: input bytes", what);
				CHECK(st.outputBytes == a.size(), "%s: output bytes", what);
				CHECK(st.storeOnlyBlocks >= 6, "%s: store-only blocks %llu", what, (unsigned long long)st.storeOnlyBlocks);
				if (precheck > 0)
					CHECK(st.precheckSkipped >= 4, "%s: pre-check skipped %llu", what, (unsigned long long)st.precheckSkipped);
				else
					CHECK(st.precheckSkipped == 0, "%s: pre-check off but skipped blocks", what);
				if (first.empty())
					first = a;
				else
					CHECK(first == a, "%s: output differs from 1 thread", what);
				if (threads == 1)
					printf("%-28s %8llu -> %8llu bytes, %llu/%llu blocks stored (%llu by pre-check, %llu store-only)\n", what,
						(unsigned long long)total, (unsigned long long)a.size(), (unsigned long long)st.storedBlocks,
						(unsigned long long)st.blocks, (unsigned long long)st.precheckSkipped, (unsigned long long)st.storeOnlyBlocks);
			}
		}
	}
	// garbage must not open
	std::vector<uint8_t> junk(1000, 7);
	CHECK(ZArchiveReader::OpenFromMemory(junk.data(), junk.size()) == nullptr, "junk opened");

	if (g_failures)
	{
		printf("%d failure(s)\n", g_failures);
		return 1;
	}
	puts("All round trips OK");
	return 0;
}
