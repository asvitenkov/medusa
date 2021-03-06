﻿#include "sqlite3_db.hpp"

#include <medusa/module.hpp>
#include <medusa/log.hpp>
#include <medusa/util.hpp>

Sqlite3Database::Sqlite3Database(void)
{
}

Sqlite3Database::~Sqlite3Database(void)
{
  // FIXME(wisk): wait for locks
}

bool Sqlite3Database::_ConfigureDatabase(void)
{
  return false;
}

bool Sqlite3Database::_CreateTable(void)
{
  m_spStorage->sync_schema();
  m_spStorage->insert<FileInformationTable>({ {}, EndianUnknown, 0x0, 0x0, 0x0 });
  return true;
}

bool Sqlite3Database::_ConvertIdToAddress(int Id, OffsetType Offset, Address& rAddress) const
{
  using namespace sqlite_orm;
  try
  {
    auto AddrTbl = m_spStorage->select(columns(
      &MemoryAreaTable::BaseAddressType,
      &MemoryAreaTable::BaseAddressBase,
      &MemoryAreaTable::BaseAddressOffset,
      &MemoryAreaTable::BaseAddressBaseSize,
      &MemoryAreaTable::BaseAddressOffsetSize),
      where(c(&MemoryAreaTable::Id) == Id));
    if (AddrTbl.size() != 1)
      return false;
    auto const& AddrTuple = AddrTbl[0];
    rAddress = Address(
      static_cast<Address::Type>(std::get<0>(AddrTuple)),
      std::get<1>(AddrTuple), std::get<2>(AddrTuple),
      std::get<3>(AddrTuple), std::get<4>(AddrTuple));
    return true;
  }
  catch (std::exception& e)
  {
    Log::Write("db_sqlite3").Level(LogError) << e.what() << LogEnd;
  }
  return false;
}

bool Sqlite3Database::_ConvertAddressToId(Address const& rAddress, int& rId, OffsetType& rOffset) const
{
  using namespace sqlite_orm;

  try
  {
    //auto AddrType = rAddress.GetAddressingType();
    //switch (AddrType)
    //{
    //case Address::PhysicalType:
    //  break;
    //case Address::LinearType:
    //  break;
    //case Address::RelativeType:
    //{
    //  ImageBaseType ImgBase;
    //  if (!GetImageBase(ImgBase))
    //    return false;

    //  break;
    //}
    //case Address::LogicalType:
    //  break;

    //default:
    //  return false;
    //}
  }
  catch (std::exception& e)
  {
    Log::Write("db_sqlite3").Level(LogError) << e.what() << LogEnd;
  }
  return false;
}

bool Sqlite3Database::_ConvertAddressToId(Address const& rAddress, int& rId, OffsetType& rOffset, OffsetType& rMemoryAreaOffset, u32& rMemoryAreaSize) const
{
  try
  {

  }
  catch (std::exception& e)
  {
    Log::Write("db_sqlite3").Level(LogError) << e.what() << LogEnd;
  }
  return false;
}

bool Sqlite3Database::_GetNextMemoryAreaId(int Id, int& rNextId) const
{
  try
  {

  }
  catch (std::exception& e)
  {
    Log::Write("db_sqlite3").Level(LogError) << e.what() << LogEnd;
  }
  return false;
}

bool Sqlite3Database::_GetPreviousMemoryAreaId(int Id, int& rPreviousId) const
{
  try
  {

  }
  catch (std::exception& e)
  {
    Log::Write("db_sqlite3").Level(LogError) << e.what() << LogEnd;
  }
  return false;
}

bool Sqlite3Database::_AddCellDataToCache(int MemoryAreaId, OffsetType MemoryAreaOffset, CellData const& rCellData)
{
  return false;
}

bool Sqlite3Database::_FlushCellDataCache(void) const
{
  return false;
}

bool Sqlite3Database::_GetCellDataFromCache(int MemoryAreaId, OffsetType MemoryOffsetType, CellData& rCellData) const
{
  return false;
}

bool Sqlite3Database::_AddLabelToCache(int MemoryAreaId, OffsetType MemoryAreaOffset, Label const & rLabel)
{
  return false;
}

bool Sqlite3Database::_FlushLabelCache(void) const
{
  return false;
}

std::string Sqlite3Database::GetName(void) const
{
  return "SQLite3";
}

std::string Sqlite3Database::GetExtension(void) const
{
  return ".sqlite";
}

bool Sqlite3Database::IsCompatible(Path const &rDatabasePath) const
{
  return false;
}

bool Sqlite3Database::_FileExists(Path const &rFilePath)
{
  return boost::filesystem::exists(rFilePath);
}

bool Sqlite3Database::_FileRemoves(Path const &rFilePath)
{
  return boost::filesystem::remove(rFilePath);
}

bool Sqlite3Database::Open(Path const &rDatabasePath)
{
  try
  {
    m_spStorage = std::make_shared<StorageType>(MakeStorage(rDatabasePath.string()));
  }
  catch (std::runtime_error const&)
  {
    return false;
  }
  return true;
}

bool Sqlite3Database::Create(Path const &rDatabasePath, bool Force)
{
  try
  {
    m_spStorage = std::make_shared<StorageType>(MakeStorage(rDatabasePath.string()));
    if (!_CreateTable())
      return false;
  }
  catch (std::runtime_error const&)
  {
    return false;
  }
  return true;
}

bool Sqlite3Database::Flush(void)
{
  return false;
}

bool Sqlite3Database::Close(void)
{
  return false;
}

bool Sqlite3Database::RegisterArchitectureTag(Tag ArchitectureTag)
{
  return false;
}

bool Sqlite3Database::UnregisterArchitectureTag(Tag ArchitectureTag)
{
  return false;
}

std::vector<Tag> Sqlite3Database::GetArchitectureTags(void) const
{
  return {};
}

bool Sqlite3Database::SetArchitecture(Address const &rAddress, Tag ArchitectureTag, u8 Mode,
  Database::SetArchitectureModeType SetArchMode)
{
  return false;
}

bool Sqlite3Database::GetImageBase(ImageBaseType& rImageBase) const
{
  using namespace sqlite_orm;
  auto ImgBase = m_spStorage->select(&FileInformationTable::ImageBase);
  if (ImgBase.size() != 1)
    return false;
  rImageBase = ImgBase[0];
  return true;
}

bool Sqlite3Database::SetImageBase(ImageBaseType ImageBase)
{
  using namespace sqlite_orm;
  m_spStorage->update_all(set(
    c(&FileInformationTable::ImageBase) = ImageBase
  ));
  return true;
}

bool Sqlite3Database::GetMemoryArea(Address const &rAddress, MemoryArea& rMemArea) const
{
  try
  {
    using namespace sqlite_orm;
    auto MemAreaId = m_spStorage->select(&MemoryAreaTable::Id, where(
      c(&MemoryAreaTable::BaseAddressBase) == rAddress.GetBase() &&
      c(&MemoryAreaTable::BaseAddressOffset) == rAddress.GetOffset()
      ));
    if (MemAreaId.size() != 1)
      return false;
    auto MemAreaTbl = m_spStorage->get<MemoryAreaTable>(MemAreaId.front());
    return MemAreaTbl.ToMemoryArea(rMemArea);
  }
  catch (std::exception const& e)
  {
    Log::Write("db_sqlite3").Level(LogError) << "exception: " << e.what() << LogEnd;
    return false;
  }

  return true;
}


void Sqlite3Database::ForEachMemoryArea(MemoryAreaCallback Callback) const
{
  for (auto const& rMemAreaTbl : m_spStorage->iterate<MemoryAreaTable>())
  {
    MemoryArea TmpMemArea;
    if (!rMemAreaTbl.ToMemoryArea(TmpMemArea))
      continue;
    Callback(TmpMemArea);
  }
}

bool Sqlite3Database::AddMemoryArea(MemoryArea const& rMemArea)
{
  auto const& rBaseAddr = rMemArea.GetBaseAddress();
  MemoryAreaTable MemAreaTbl{
    -1, 
    rMemArea.GetName(),
    rMemArea.GetType(),
    static_cast<std::underlying_type<MemoryArea::Type>::type>(rMemArea.GetAccess()),
    rMemArea.GetArchitectureTag(),
    rMemArea.GetArchitectureMode(),
    rMemArea.GetFileOffset(),
    rMemArea.GetFileSize(),
    rBaseAddr.GetAddressingType(), rBaseAddr.GetBase(), rBaseAddr.GetOffset(), rBaseAddr.GetBaseSize(), rBaseAddr.GetOffsetSize(),
    rMemArea.GetSize()
  };
  m_spStorage->insert(MemAreaTbl);
  return true;
}

bool Sqlite3Database::RemoveMemoryArea(MemoryArea const& rMemArea)
{
  using namespace sqlite_orm;
  m_spStorage->remove_all<MemoryAreaTable>(where(
    c(&MemoryAreaTable::BaseAddressBase)   == rMemArea.GetBaseAddress().GetBase()   &&
    c(&MemoryAreaTable::BaseAddressOffset) == rMemArea.GetBaseAddress().GetOffset()
  ));
  return true;
}

bool Sqlite3Database::MoveMemoryArea(MemoryArea const& rMemArea, Address const& rBaseAddress)
{
  return false;
}

bool Sqlite3Database::GetDefaultAddressingType(Address::Type& rAddressType) const
{
  using namespace sqlite_orm;
  auto DefAddrTy = m_spStorage->select(&FileInformationTable::DefaultAddressingType);
  if (DefAddrTy.size() != 1)
    return false;
  rAddressType = static_cast<Address::Type>(DefAddrTy[0]);
  return true;
}

bool Sqlite3Database::SetDefaultAddressingType(Address::Type AddressType)
{
  using namespace sqlite_orm;
  m_spStorage->update_all(
    set(c(&FileInformationTable::DefaultAddressingType) = static_cast<u8>(AddressType))
  );
  return true;
}

// Implemented possibilities:
// - Logical Address → Linear Address → (Relative Address → Physical address) WIP
// - (Physical Address → Relative Address) → Linear Address → Logical Address (might not normalized)
// 
// See document.hpp Document::Translate
bool Sqlite3Database::TranslateAddress(Address const& rAddress, Address::Type ToConvert, Address& rTranslatedAddress) const
{
  using namespace sqlite_orm;
  try
  {
    switch (rAddress.GetAddressingType())
    {
    case Address::LinearType:
      break;
    }
    return true;
  }
  catch (std::exception& e)
  {
    Log::Write("db_sqlite3").Level(LogError) << e.what() << LogEnd;
  }
  return false;
}

bool Sqlite3Database::GetFirstAddress(Address &rAddress) const
{
  return false;
}

bool Sqlite3Database::GetLastAddress(Address &rAddress) const
{
  return false;
}

bool Sqlite3Database::MoveAddress(Address const &rAddress, Address &rMovedAddress, s64 Displacement) const
{
  return false;
}

bool Sqlite3Database::ConvertAddressToPosition(Address const &rAddress, u32 &rPosition) const
{
  return false;
}

bool Sqlite3Database::ConvertPositionToAddress(u32 Position, Address &rAddress) const
{
  return false;
}

bool Sqlite3Database::AddLabel(Address const &rAddress, Label const &rLabel)
{
  int Id = 0;
  u64 Off = 0;
  if (!_ConvertAddressToId(rAddress, Id, Off))
    return false;
  MemoryAreaRelated MemAreaRel{ Id, Off };
  LabelTable LblTbl{ MemAreaRel, rLabel };
  m_spStorage->insert(LblTbl);
  return true;
}

bool Sqlite3Database::RemoveLabel(Address const &rAddress)
{
  return false;
}

bool Sqlite3Database::GetLabel(Address const &rAddress, Label &rLabel) const
{
  return false;
}

bool Sqlite3Database::GetLabelAddress(Label const &rLabel, Address &rAddress) const
{
  return false;
}

void Sqlite3Database::ForEachLabel(LabelCallback Callback)
{
}

bool Sqlite3Database::AddCrossReference(Address const &rTo, Address const &rFrom)
{
  return false;
}

bool Sqlite3Database::RemoveCrossReference(Address const &rFrom)
{
  return false;
}

bool Sqlite3Database::GetCrossReferenceFrom(Address const &rTo, Address::Vector &rFrom) const
{
  return false;
}

bool Sqlite3Database::GetCrossReferenceTo(Address const &rFrom, Address::Vector &rTo) const
{
  return false;
}

MultiCell::SPType Sqlite3Database::GetMultiCell(Address const &rAddress) const
{
  return false;
}

bool Sqlite3Database::SetMultiCell(Address const &rAddress, MultiCell::SPType spMultiCell)
{
  return false;
}

bool Sqlite3Database::DeleteMultiCell(Address const &rAddress)
{
  return false;
}

bool Sqlite3Database::GetCellData(Address const &rAddress, CellData &rCellData) const
{
  return false;
}

bool Sqlite3Database::SetCellData(Address const &rAddress, CellData const &rCellData, Address::Vector &rDeletedCellAddresses, bool Force)
{
  return false;
}

bool Sqlite3Database::DeleteCellData(Address const &rAddress)
{
  return false;
}

bool Sqlite3Database::GetComment(Address const &rAddress, std::string &rComment) const
{
  return false;
}

bool Sqlite3Database::SetComment(Address const &rAddress, std::string const &rComment)
{
  return false;
}

bool Sqlite3Database::GetValueDetail(Id ConstId, ValueDetail &rConstDtl) const
{
  return false;
}

bool Sqlite3Database::SetValueDetail(Id ConstId, ValueDetail const &rConstDtl)
{
  return false;
}

bool Sqlite3Database::GetFunctionDetail(Id FuncId, FunctionDetail &rFuncDtl) const
{
  return false;
}

bool Sqlite3Database::SetFunctionDetail(Id FuncId, FunctionDetail const &rFuncDtl)
{
  return false;
}

bool Sqlite3Database::GetStructureDetail(Id StructId, StructureDetail &rStructDtl) const
{
  return false;
}

bool Sqlite3Database::SetStructureDetail(Id StructId, StructureDetail const &rStructDtl)
{
  return false;
}

bool Sqlite3Database::RetrieveDetailId(Address const &rAddress, u8 Index, Id &rDtlId) const
{
  return false;
}

bool Sqlite3Database::BindDetailId(Address const &rAddress, u8 Index, Id DtlId)
{
  return false;
}

bool Sqlite3Database::UnbindDetailId(Address const &rAddress, u8 Index)
{
  return false;
}