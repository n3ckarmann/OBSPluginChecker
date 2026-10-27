#include "pe-file.hpp"

#include <QFile>

#include <cstdint>
#include <cstring>

namespace {

struct Section {
	uint32_t virtualAddress;
	uint32_t virtualSize;
	uint32_t rawOffset;
	uint32_t rawSize;
};

class Reader {
public:
	Reader(const uchar *data, qint64 size) : data_(data), size_(size) {}

	bool has(qint64 offset, qint64 len) const { return offset >= 0 && len >= 0 && offset + len <= size_; }

	uint16_t u16(qint64 offset) const
	{
		uint16_t v = 0;
		if (has(offset, 2))
			std::memcpy(&v, data_ + offset, 2);
		return v;
	}

	uint32_t u32(qint64 offset) const
	{
		uint32_t v = 0;
		if (has(offset, 4))
			std::memcpy(&v, data_ + offset, 4);
		return v;
	}

	QString str(qint64 offset) const
	{
		if (!has(offset, 1))
			return {};
		qint64 end = offset;
		while (end < size_ && data_[end] && end - offset < 512)
			end++;
		return QString::fromLatin1(reinterpret_cast<const char *>(data_ + offset), int(end - offset));
	}

private:
	const uchar *data_;
	qint64 size_;
};

qint64 rva_to_offset(const QList<Section> &sections, uint32_t rva)
{
	for (const Section &s : sections) {
		uint32_t span = qMax(s.virtualSize, s.rawSize);
		if (rva >= s.virtualAddress && rva < s.virtualAddress + span)
			return qint64(rva - s.virtualAddress) + s.rawOffset;
	}
	return -1;
}

} // namespace

PeInfo read_pe(const QString &path)
{
	PeInfo info;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() < 0x40)
		return info;
	const uchar *data = file.map(0, file.size());
	if (!data)
		return info;
	Reader r(data, file.size());

	if (r.u16(0) != 0x5a4d) // "MZ"
		return info;
	qint64 pe = r.u32(0x3c);
	if (r.u32(pe) != 0x00004550) // "PE\0\0"
		return info;
	qint64 coff = pe + 4;
	int sectionCount = r.u16(coff + 2);
	qint64 optional = coff + 20;
	qint64 optionalSize = r.u16(coff + 16);
	uint16_t magic = r.u16(optional);
	qint64 dirs;
	if (magic == 0x20b) // PE32+
		dirs = optional + 112;
	else if (magic == 0x10b) // PE32
		dirs = optional + 96;
	else
		return info;

	QList<Section> sections;
	qint64 table = optional + optionalSize;
	for (int i = 0; i < sectionCount && i < 96; i++) {
		qint64 s = table + i * 40;
		if (!r.has(s, 40))
			break;
		sections.append({r.u32(s + 12), r.u32(s + 8), r.u32(s + 20), r.u32(s + 16)});
	}
	info.valid = true;

	qint64 exportDir = rva_to_offset(sections, r.u32(dirs));
	if (r.u32(dirs + 4) && exportDir >= 0) {
		uint32_t count = qMin<uint32_t>(r.u32(exportDir + 24), 100000);
		qint64 names = rva_to_offset(sections, r.u32(exportDir + 32));
		for (uint32_t i = 0; names >= 0 && i < count; i++)
			info.exports.append(r.str(rva_to_offset(sections, r.u32(names + 4 * i))));
	}

	qint64 importDir = rva_to_offset(sections, r.u32(dirs + 8));
	if (r.u32(dirs + 12) && importDir >= 0) {
		for (int i = 0; i < 4096; i++) {
			qint64 d = importDir + i * 20;
			if (!r.has(d, 20) || (r.u32(d) == 0 && r.u32(d + 12) == 0))
				break;
			QString name = r.str(rva_to_offset(sections, r.u32(d + 12)));
			if (!name.isEmpty())
				info.imports.append(name);
		}
	}
	return info;
}
