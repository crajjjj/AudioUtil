#pragma once

// RE::BSResourceNiBinaryStream torn down by the game's destructor alone. Use this,
// never a bare RE::BSResourceNiBinaryStream.
//
// CommonLib 7.1's ~BSResourceNiBinaryStream() calls the game's destructor
// (69638 / 71016), which closes and releases `stream` (deleting it at the last
// reference) but leaves the pointer in the field; the compiler then destroys the
// BSTSmartPointer member and releases the same stream again. That is a
// use-after-free on every successful open: heap corruption, or a CTD when the
// freed page is already gone (a user crash log in FolderCache's explicit-file
// audit, 0.9.30). Fixed upstream in alandtse/CommonLibSSE-NG #453, 300 commits past
// our submodule. The anonymous union keeps the compiler from running any C++
// destructor, so this stays correct after a bump as well.
class ResourceStream
{
public:
	explicit ResourceStream(const char* a_dataRelPath) :
		stream(a_dataRelPath)
	{}

	~ResourceStream()
	{
		using func_t = void(RE::BSResourceNiBinaryStream*);
		static REL::Relocation<func_t> dtor{ RELOCATION_ID(69638, 71016) };
		dtor(std::addressof(stream));
	}

	ResourceStream(const ResourceStream&) = delete;
	ResourceStream& operator=(const ResourceStream&) = delete;

	union
	{
		RE::BSResourceNiBinaryStream stream;
	};
};
