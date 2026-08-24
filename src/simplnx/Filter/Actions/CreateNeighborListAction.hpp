#pragma once

#include "simplnx/DataStructure/NeighborList.hpp"
#include "simplnx/Filter/Output.hpp"

#include "simplnx/simplnx_export.hpp"

#include <string>

namespace nx::core
{
/**
 * @class CreateNeighborListAction
 * @brief Action for creating NeighborList arrays in a DataStructure
 */
class SIMPLNX_EXPORT CreateNeighborListAction : public IDataCreationAction
{
public:
  CreateNeighborListAction() = delete;

  /**
   * @brief Constructs a CreateNeighborListAction.
   * @param type The data type of the NeighborList
   * @param tupleShape The tuple shape of the NeighborList
   * @param path The path where the NeighborList will be created
   * @param dataFormat The data store format override. Empty string means "Automatic"
   *                   (let the format resolver decide). A non-empty value skips the
   *                   resolver and requests the specified format directly, subject to
   *                   the unstructured-geometry in-core gate.
   */
  CreateNeighborListAction(DataType type, const ShapeType& tupleShape, const DataPath& path, std::string dataFormat = "");

  ~CreateNeighborListAction() noexcept override;

  CreateNeighborListAction(const CreateNeighborListAction&) = delete;
  CreateNeighborListAction(CreateNeighborListAction&&) noexcept = delete;
  CreateNeighborListAction& operator=(const CreateNeighborListAction&) = delete;
  CreateNeighborListAction& operator=(CreateNeighborListAction&&) noexcept = delete;

  /**
   * @brief Applies this action's change to the given DataStructure in the given mode.
   * Returns any warnings/errors. On error, DataStructure is not guaranteed to be consistent.
   * @param dataStructure The DataStructure to modify
   * @param mode The mode (Preflight or Execute)
   * @return Result<> Result with any errors or warnings
   */
  Result<> apply(DataStructure& dataStructure, Mode mode) const override;

  /**
   * @brief Returns a copy of the action.
   * @return UniquePointer A unique pointer to the cloned action
   */
  UniquePointer clone() const override;

  /**
   * @brief Returns the DataType of the DataArray to be created.
   * @return DataType
   */
  DataType type() const;

  /**
   * @brief Returns the shape of tuples for the NeighborList to be created.
   * @return usize
   */
  const ShapeType& tupleShape() const;

  /**
   * @brief Returns the path of the DataArray to be created.
   * @return DataPath
   */
  DataPath path() const;

  /**
   * @brief Returns all of the DataPaths to be created.
   * @return std::vector<DataPath>
   */
  std::vector<DataPath> getAllCreatedPaths() const override;

  /**
   * @brief Returns the data store format override for this action.
   *
   * Empty string means "Automatic" -- the format resolver decides. A non-empty
   * value skips the resolver and requests the specified format directly (subject
   * to the unstructured-geometry in-core gate), allowing individual filters to
   * override the global format policy.
   *
   * @return The data format string
   */
  std::string dataFormat() const;

private:
  DataType m_Type;
  ShapeType m_TupleShape;
  std::string m_DataFormat = "";
};
} // namespace nx::core
