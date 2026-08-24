#include "ArrayCalculator.hpp"

#include "simplnx/Common/Result.hpp"
#include "simplnx/Common/TypesUtility.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/Utilities/DataGroupUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>

using namespace nx::core;

// ===========================================================================
// Anonymous namespace: ParsedItem, helper functions, functors
// ===========================================================================
namespace
{

// ---------------------------------------------------------------------------
// Bounded chunk size (in tuples' worth of elements, roughly) used throughout this file for the
// streaming evaluator's bulk copyIntoBuffer/copyFromBuffer calls. Every per-chunk buffer used by
// the evaluator is sized off of this constant (scaled down for arrays with many components), never
// off of the array's total tuple count -- that bound is what lets the evaluator stream an
// out-of-core array as a handful of hyperslab reads/writes rather than one round-trip per voxel,
// without allocating memory proportional to the input size.
// ---------------------------------------------------------------------------
constexpr usize k_ChunkSize = 65536;

// ---------------------------------------------------------------------------
// Intermediate representation used between parsing and shunting-yard.
// Includes parentheses and commas that the final RpnItem list does not.
// ---------------------------------------------------------------------------
struct ParsedItem
{
  enum class Kind
  {
    Scalar,
    ArrayRef,
    Operator,
    LParen,
    RParen,
    Comma,
    ComponentExtract,
    TupleComponentExtract
  } kind;

  // Scalar
  float64 scalarValue = 0.0;

  // ArrayRef: metadata for validation (no data allocated)
  DataPath arrayPath;
  DataType sourceDataType = DataType::float64;
  std::vector<usize> arrayTupleShape;
  std::vector<usize> arrayCompShape;

  // Operator
  const OperatorDef* op = nullptr;
  bool isNegativePrefix = false;

  // ComponentExtract / TupleComponentExtract
  usize componentIndex = std::numeric_limits<usize>::max();
  usize tupleIndex = std::numeric_limits<usize>::max();
};

// ---------------------------------------------------------------------------
// The static unary negative OperatorDef (not in the registry)
// ---------------------------------------------------------------------------
const OperatorDef& getUnaryNegativeOp()
{
  static const OperatorDef s_UnaryNeg = {"neg", OperatorDef::UnaryPrefix, 4, 1, OperatorDef::Right, OperatorDef::None, [](double x) { return -x; }, nullptr};
  return s_UnaryNeg;
}

// ---------------------------------------------------------------------------
// Look up an operator/function in the registry by exact token match
// ---------------------------------------------------------------------------
const OperatorDef* findOperatorByToken(const std::string& token)
{
  const auto& registry = getOperatorRegistry();
  for(const auto& opDef : registry)
  {
    if(opDef.token == token)
    {
      return &opDef;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Map single-character TokenType to the operator registry token string
// ---------------------------------------------------------------------------
const OperatorDef* operatorDefForSymbolToken(TokenType type)
{
  switch(type)
  {
  case TokenType::Plus:
    return findOperatorByToken("+");
  case TokenType::Minus:
    return findOperatorByToken("-");
  case TokenType::Star:
    return findOperatorByToken("*");
  case TokenType::Slash:
    return findOperatorByToken("/");
  case TokenType::Caret:
    return findOperatorByToken("^");
  case TokenType::Percent:
    return findOperatorByToken("%");
  default:
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Search the entire DataStructure for IDataArrays with a given name.
// Returns all DataPaths where a matching array is found.
// ---------------------------------------------------------------------------
std::vector<DataPath> findArraysByName(const DataStructure& ds, const std::string& name)
{
  std::vector<DataPath> results;

  // Search recursively from the root
  auto allPaths = GetAllChildDataPathsRecursive(ds, DataPath{}, DataObject::Type::DataArray);
  if(allPaths.has_value())
  {
    for(const auto& path : allPaths.value())
    {
      if(path.getTargetName() == name)
      {
        results.push_back(path);
      }
    }
  }

  return results;
}

// ---------------------------------------------------------------------------
// Check whether the previous ParsedItem is a binary operator
// ---------------------------------------------------------------------------
bool isBinaryOp(const ParsedItem& item)
{
  return item.kind == ParsedItem::Kind::Operator && item.op != nullptr && item.op->kind == OperatorDef::BinaryInfix && !item.isNegativePrefix;
}

// ---------------------------------------------------------------------------
// WrapFunctionArguments: for each function call in the parsed item list,
// wrap each comma-separated argument in extra parentheses so that the
// shunting-yard algorithm processes them correctly.
//
// Example: sin(a + b) stays as sin((a + b))
//          log(a, b)  becomes log((a), (b))
// ---------------------------------------------------------------------------
void wrapFunctionArguments(std::vector<ParsedItem>& items)
{
  std::vector<ParsedItem> out;
  out.reserve(items.size() * 2);

  for(size_t i = 0; i < items.size(); ++i)
  {
    const auto& item = items[i];

    // Detect: Function operator followed by LParen
    if(item.kind == ParsedItem::Kind::Operator && item.op != nullptr && item.op->kind == OperatorDef::Function && i + 1 < items.size() && items[i + 1].kind == ParsedItem::Kind::LParen)
    {
      // Copy function and '('
      out.push_back(item);
      out.push_back(items[++i]);
      int depth = 1;
      size_t argStart = out.size();

      // Process until matching ')'
      for(++i; i < items.size() && depth > 0; ++i)
      {
        const auto& cur = items[i];
        if(cur.kind == ParsedItem::Kind::LParen)
        {
          ++depth;
          out.push_back(cur);
        }
        else if(cur.kind == ParsedItem::Kind::RParen)
        {
          --depth;
          if(depth == 0)
          {
            // Close last argument with wrapping parens
            ParsedItem lp;
            lp.kind = ParsedItem::Kind::LParen;
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(argStart), lp);

            ParsedItem rp;
            rp.kind = ParsedItem::Kind::RParen;
            out.push_back(rp);
            break;
          }
          out.push_back(cur);
        }
        else if(cur.kind == ParsedItem::Kind::Comma && depth == 1)
        {
          // End this argument, copy comma, start next argument
          ParsedItem rp;
          rp.kind = ParsedItem::Kind::RParen;
          out.push_back(rp);

          out.push_back(cur);

          ParsedItem lp;
          lp.kind = ParsedItem::Kind::LParen;
          out.push_back(lp);
        }
        else
        {
          out.push_back(cur);
        }
      }
      --i; // we consumed the ')' in the inner loop
    }
    else
    {
      out.push_back(item);
    }
  }

  items.swap(out);
}

// ---------------------------------------------------------------------------
// Functor reading a single element (any numeric source type) as a float64, by type-dispatched
// dynamic_cast + at(). Only used by the O(1) reduction pre-pass below (resolving a
// TupleComponentExtract's operand, or a whole-expression scalar result) -- never from inside the
// bounded per-chunk streaming loop, so a single-element round trip here costs nothing relative to
// the size of the dataset.
// ---------------------------------------------------------------------------
struct ReadSingleElementFunctor
{
  template <typename T>
  float64 operator()(const IDataArray& sourceArray, usize flatIndex)
  {
    const auto& typedSource = dynamic_cast<const DataArray<T>&>(sourceArray);
    return static_cast<float64>(typedSource.at(flatIndex));
  }
};

// ---------------------------------------------------------------------------
// Functor reading a bounded contiguous run of `destBuffer.size()` elements (any numeric source
// type) into `destBuffer` as float64. The scratch buffer holding the source's native type is sized
// to the caller's chunk (never the array's total element count), so an out-of-core source is
// touched with one bulk hyperslab read per chunk instead of one round-trip per voxel.
// ---------------------------------------------------------------------------
struct ReadChunkToFloat64Functor
{
  template <typename T>
  void operator()(const IDataArray& sourceArray, usize startIndex, nonstd::span<float64> destBuffer)
  {
    const auto& typedSource = dynamic_cast<const DataArray<T>&>(sourceArray);
    const auto& sourceStore = typedSource.getDataStoreRef();
    const usize count = destBuffer.size();

    // std::vector<bool> packs to bits and has no contiguous .data(); make_unique<T[]> sidesteps
    // that specialization for the T=bool instantiation of this template.
    auto rawBuf = std::make_unique<T[]>(count);
    sourceStore.copyIntoBuffer(startIndex, nonstd::span<T>(rawBuf.get(), count));
    for(usize i = 0; i < count; i++)
    {
      destBuffer[i] = static_cast<float64>(rawBuf[i]);
    }
  }
};

// ---------------------------------------------------------------------------
// Functor casting a bounded contiguous run of float64 values down to the output array's own
// numeric type and writing them. The scratch buffer holding the cast values is sized to the
// caller's chunk, so a non-float64 out-of-core output is written with one bulk hyperslab write per
// chunk instead of one round-trip per voxel.
// ---------------------------------------------------------------------------
struct WriteChunkFromFloat64Functor
{
  template <typename T>
  void operator()(DataStructure& ds, const DataPath& outputPath, usize startIndex, nonstd::span<const float64> srcBuffer)
  {
    auto& outputStore = ds.getDataRefAs<DataArray<T>>(outputPath).getDataStoreRef();
    const usize count = srcBuffer.size();

    // std::vector<bool> packs to bits and has no contiguous .data(); make_unique<T[]> sidesteps
    // that specialization for the T=bool instantiation of this template.
    auto writeBuf = std::make_unique<T[]>(count);
    for(usize i = 0; i < count; i++)
    {
      writeBuf[i] = static_cast<T>(srcBuffer[i]);
    }
    outputStore.copyFromBuffer(startIndex, nonstd::span<const T>(writeBuf.get(), count));
  }
};

// ---------------------------------------------------------------------------
// Functor filling an output array of any numeric type with a single scalar value. Used when the
// entire infix expression collapses to one float64 value (all-constant arithmetic, or a
// Array[T, C] / (expr)[T, C] extraction that is the whole expression) -- the value is computed
// once and broadcast via the store's own fill(), never via a per-element loop here.
// ---------------------------------------------------------------------------
struct FillScalarResultFunctor
{
  template <typename T>
  void operator()(DataStructure& ds, const DataPath& outputPath, float64 scalarValue)
  {
    auto& output = ds.getDataRefAs<DataArray<T>>(outputPath);
    output.fill(static_cast<T>(scalarValue));
  }
};

// ---------------------------------------------------------------------------
// For every position i in a valid RPN (postfix) sequence, computes the index at which the
// subexpression producing items[i]'s value begins. Reverse-polish notation has the property that
// every subexpression is a contiguous run of tokens ending at the token that combines it, so a
// single forward pass tracking, for each value currently pending on a simulated stack, the index
// where its subtree started, is enough. This lets the TupleComponentExtract reduction pass below
// carve a subexpression's tokens out of the RPN list, and lets the single-element evaluator find
// where a binary operator's left operand ends (immediately before its right operand's own start).
// ---------------------------------------------------------------------------
std::vector<usize> computeSpanStarts(const std::vector<RpnItem>& rpn)
{
  std::vector<usize> spanStart(rpn.size(), 0);
  std::vector<usize> pending; // start-indices of values currently on the simulated stack

  for(usize i = 0; i < rpn.size(); i++)
  {
    const RpnItem& item = rpn[i];
    const bool isBinaryOperator = item.type == RpnItem::Type::Operator && item.op != nullptr && item.op->numArgs == 2;
    const bool consumesOneOperand = (item.type == RpnItem::Type::Operator && !isBinaryOperator) || item.type == RpnItem::Type::ComponentExtract || item.type == RpnItem::Type::TupleComponentExtract;

    usize myStart = i;
    if(isBinaryOperator)
    {
      pending.pop_back(); // right operand's start -- the span still begins at the left operand
      myStart = pending.back();
      pending.pop_back();
    }
    else if(consumesOneOperand)
    {
      myStart = pending.back();
      pending.pop_back();
    }
    // Scalar / ArrayRef are leaves: myStart == i (already set above).

    spanStart[i] = myStart;
    pending.push_back(myStart);
  }

  return spanStart;
}

// ---------------------------------------------------------------------------
// Recursively evaluates the value produced by rpn[nodeIndex] (and everything under it) at a single
// (tupleIdx, compIdx) target, reading at most one element from each ArrayRef leaf involved -- O(1)
// per source array regardless of how many tuples/components it has. compIdx is threaded down
// unchanged through Scalar/ArrayRef/Operator nodes (they all act on whichever single component the
// caller asked for), but a ComponentExtract node OVERRIDES it with its own fixed literal index for
// everything beneath it, matching the elementwise semantics the bounded per-chunk pass uses for the
// same node type. Used only by the TupleComponentExtract reduction pass and by the whole-expression
// scalar fast path below, both of which are O(expression length), not O(data size).
// ---------------------------------------------------------------------------
Result<float64> evaluateSingleValue(const DataStructure& dataStructure, const std::vector<RpnItem>& rpn, usize nodeIndex, const std::vector<usize>& spanStart, usize tupleIdx, usize compIdx,
                                    CalculatorParameter::AngleUnits units)
{
  const RpnItem& item = rpn[nodeIndex];

  switch(item.type)
  {
  case RpnItem::Type::Scalar:
    return {item.scalarValue};

  case RpnItem::Type::ArrayRef: {
    const auto* sourceArray = dataStructure.getDataAs<IDataArray>(item.arrayPath);
    if(sourceArray == nullptr)
    {
      return MakeErrorResult<float64>(static_cast<int32>(CalculatorErrorCode::InvalidEquation),
                                      fmt::format("Internal error: array '{}' could not be resolved during evaluation.", item.arrayPath.toString()));
    }
    const usize numComps = sourceArray->getNumberOfComponents();
    const usize flatIndex = tupleIdx * numComps + compIdx;
    const float64 value = ExecuteDataFunction(ReadSingleElementFunctor{}, item.sourceDataType, *sourceArray, flatIndex);
    return {value};
  }

  case RpnItem::Type::Operator: {
    const OperatorDef* op = item.op;
    if(op == nullptr)
    {
      return MakeErrorResult<float64>(static_cast<int32>(CalculatorErrorCode::InvalidEquation), "Internal error: null operator encountered during scalar evaluation.");
    }

    if(op->numArgs == 1)
    {
      Result<float64> operandResult = evaluateSingleValue(dataStructure, rpn, nodeIndex - 1, spanStart, tupleIdx, compIdx, units);
      if(operandResult.invalid())
      {
        return operandResult;
      }
      float64 val = operandResult.value();
      if(op->trigMode == OperatorDef::ForwardTrig && units == CalculatorParameter::AngleUnits::Degrees)
      {
        val = val * (std::numbers::pi / 180.0);
      }
      float64 res = op->unaryOp(val);
      if(op->trigMode == OperatorDef::InverseTrig && units == CalculatorParameter::AngleUnits::Degrees)
      {
        res = res * (180.0 / std::numbers::pi);
      }
      return {res};
    }

    // Binary: the right operand ends immediately before this node; its own recorded span start
    // tells us where the left operand ends.
    const usize rightIdx = nodeIndex - 1;
    const usize rightStart = spanStart[rightIdx];
    const usize leftIdx = rightStart - 1;

    Result<float64> rightResult = evaluateSingleValue(dataStructure, rpn, rightIdx, spanStart, tupleIdx, compIdx, units);
    if(rightResult.invalid())
    {
      return rightResult;
    }
    Result<float64> leftResult = evaluateSingleValue(dataStructure, rpn, leftIdx, spanStart, tupleIdx, compIdx, units);
    if(leftResult.invalid())
    {
      return leftResult;
    }
    return {op->binaryOp(leftResult.value(), rightResult.value())};
  }

  case RpnItem::Type::ComponentExtract: {
    // Overrides compIdx for the ENTIRE operand subtree beneath it, regardless of what component
    // the caller was originally asking for -- e.g. in "(a + b)[1]", both 'a' and 'b' must be read
    // at component 1, not whatever component an enclosing Array[T, C] extraction wanted.
    return evaluateSingleValue(dataStructure, rpn, nodeIndex - 1, spanStart, tupleIdx, item.componentIndex, units);
  }

  case RpnItem::Type::TupleComponentExtract:
    return MakeErrorResult<float64>(static_cast<int32>(CalculatorErrorCode::InvalidEquation), "Internal error: nested TupleComponentExtract encountered during scalar evaluation.");
  }

  return MakeErrorResult<float64>(static_cast<int32>(CalculatorErrorCode::InvalidEquation), "Internal error: unrecognized RPN item type during scalar evaluation.");
}

// ---------------------------------------------------------------------------
// Per-node shape summary tracked by simulateShapes(), below: whether the node's value is a single
// broadcastable scalar (independent of any tuple index), and if not, how many components each of
// its tuples has. This never touches array data -- only getNumberOfComponents() -- so the whole
// simulation costs O(expression length), not O(data size).
// ---------------------------------------------------------------------------
struct RpnNodeShape
{
  bool isScalar = false;
  usize numComponents = 1;
};

// ---------------------------------------------------------------------------
// Aggregate result of simulating the RPN stack machine over rpn[startIndex..endIndex] using shape
// information only (no data). finalShape is the shape of the subexpression's result; maxStackDepth
// and maxComponents are the peak values needed to size the bounded per-node buffers the main
// streaming pass below pre-allocates once, before iterating chunks.
// ---------------------------------------------------------------------------
struct ShapeSimResult
{
  RpnNodeShape finalShape;
  usize maxStackDepth = 0;
  usize maxComponents = 1;
};

// ---------------------------------------------------------------------------
// Simulates rpn[startIndex..endIndex] (inclusive), a self-contained RPN subexpression, tracking
// only shape information. Reproduces the same broadcast/shape propagation rules the bounded
// evaluators below apply to actual data: a unary operator or ComponentExtract preserves its
// operand's scalar-ness; a binary operator's result is scalar only if both operands are; and
// whichever operand is NOT scalar determines the component count (matching how the elementwise
// loops broadcast a lone scalar operand against a full array). Also performs the ComponentExtract
// bounds check the original element-at-a-time evaluator applied at runtime, since Case B
// "(expr)[C]" is not bounds-checked until the operand's shape is known.
// ---------------------------------------------------------------------------
Result<ShapeSimResult> simulateShapes(const DataStructure& dataStructure, const std::vector<RpnItem>& rpn, usize startIndex, usize endIndex)
{
  ShapeSimResult out;
  std::vector<RpnNodeShape> stack;

  for(usize i = startIndex; i <= endIndex; i++)
  {
    const RpnItem& item = rpn[i];
    switch(item.type)
    {
    case RpnItem::Type::Scalar:
      stack.push_back({true, 1});
      break;

    case RpnItem::Type::ArrayRef: {
      const auto* sourceArray = dataStructure.getDataAs<IDataArray>(item.arrayPath);
      const usize numComps = (sourceArray != nullptr) ? sourceArray->getNumberOfComponents() : 1;
      stack.push_back({false, numComps});
      break;
    }

    case RpnItem::Type::Operator: {
      const OperatorDef* op = item.op;
      if(op != nullptr && op->numArgs == 2)
      {
        const RpnNodeShape right = stack.back();
        stack.pop_back();
        const RpnNodeShape left = stack.back();
        stack.pop_back();
        const bool resultScalar = left.isScalar && right.isScalar;
        const usize numComps = left.isScalar ? right.numComponents : left.numComponents;
        stack.push_back({resultScalar, resultScalar ? 1 : numComps});
      }
      else
      {
        const RpnNodeShape operand = stack.back();
        stack.pop_back();
        stack.push_back({operand.isScalar, operand.isScalar ? 1 : operand.numComponents});
      }
      break;
    }

    case RpnItem::Type::ComponentExtract: {
      const RpnNodeShape operand = stack.back();
      stack.pop_back();
      const usize numComps = operand.isScalar ? 1 : operand.numComponents;
      if(item.componentIndex >= numComps)
      {
        return MakeErrorResult<ShapeSimResult>(static_cast<int32>(CalculatorErrorCode::ComponentOutOfRange),
                                               fmt::format("Component index {} is out of range for array with {} components.", item.componentIndex, numComps));
      }
      // Extracting a single component from an operand that was already an inherent scalar (e.g.
      // "(2+3)[0]") still yields a broadcastable scalar.
      stack.push_back({operand.isScalar, 1});
      break;
    }

    case RpnItem::Type::TupleComponentExtract:
      return MakeErrorResult<ShapeSimResult>(static_cast<int32>(CalculatorErrorCode::InvalidEquation), "Internal error: unresolved TupleComponentExtract encountered during shape simulation.");
    }

    out.maxStackDepth = std::max(out.maxStackDepth, stack.size());
    out.maxComponents = std::max(out.maxComponents, stack.back().numComponents);
  }

  out.finalShape = stack.back();
  return {out};
}

// ---------------------------------------------------------------------------
// Finds the first ArrayRef within rpn[startIndex..endIndex] and returns its tuple count; returns 1
// if the subexpression contains no array at all (a pure scalar/constant sub-expression, matching
// the single-tuple shape such an expression evaluates to). Used only to bounds-check a
// TupleComponentExtract's literal tuple index against its operand's actual tuple count.
// ---------------------------------------------------------------------------
usize findOperandNumTuples(const DataStructure& dataStructure, const std::vector<RpnItem>& rpn, usize startIndex, usize endIndex)
{
  for(usize i = startIndex; i <= endIndex; i++)
  {
    if(rpn[i].type == RpnItem::Type::ArrayRef)
    {
      const auto* sourceArray = dataStructure.getDataAs<IDataArray>(rpn[i].arrayPath);
      if(sourceArray != nullptr)
      {
        return sourceArray->getNumberOfTuples();
      }
    }
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Reduction pre-pass: resolves every TupleComponentExtract ("Array[T, C]" or "(expr)[T, C]") down
// to a cached float64 Scalar RPN item, in place. Each resolution touches its operand subexpression
// with a bounded, O(1)-per-array single-element read (via evaluateSingleValue) instead of
// materializing the whole subexpression -- this is what lets an out-of-core dataset be indexed by
// a literal tuple/component pair without an O(n_cells) buffer anywhere.
//
// TupleComponentExtract items are resolved leftmost-first. Reverse-polish subexpressions nest
// contiguously, so if two TupleComponentExtract items are nested (one's operand subexpression
// contains the other), the inner one necessarily has a smaller index than the outer one; always
// picking the smallest remaining index therefore always resolves the innermost pending extraction
// first, guaranteeing every operand span handed to evaluateSingleValue() is itself already free of
// TupleComponentExtract items.
// ---------------------------------------------------------------------------
Result<> resolveTupleComponentExtracts(const DataStructure& dataStructure, std::vector<RpnItem>& rpn, CalculatorParameter::AngleUnits units)
{
  while(true)
  {
    usize tceIndex = rpn.size();
    for(usize i = 0; i < rpn.size(); i++)
    {
      if(rpn[i].type == RpnItem::Type::TupleComponentExtract)
      {
        tceIndex = i;
        break;
      }
    }
    if(tceIndex == rpn.size())
    {
      break; // no TupleComponentExtract items remain -- reduction complete
    }

    const std::vector<usize> spanStart = computeSpanStarts(rpn);
    const usize operandIdx = tceIndex - 1;
    const usize operandStart = spanStart[operandIdx];

    const usize tupleIdx = rpn[tceIndex].tupleIndex;
    const usize compIdx = rpn[tceIndex].componentIndex;

    const usize numTuplesOperand = findOperandNumTuples(dataStructure, rpn, operandStart, operandIdx);
    if(tupleIdx >= numTuplesOperand)
    {
      return MakeErrorResult(static_cast<int32>(CalculatorErrorCode::TupleOutOfRange), fmt::format("Tuple index {} is out of range for array with {} tuples.", tupleIdx, numTuplesOperand));
    }

    Result<ShapeSimResult> shapeResult = simulateShapes(dataStructure, rpn, operandStart, operandIdx);
    if(shapeResult.invalid())
    {
      return ConvertResult(std::move(shapeResult));
    }
    const usize numCompsOperand = shapeResult.value().finalShape.isScalar ? 1 : shapeResult.value().finalShape.numComponents;
    if(compIdx >= numCompsOperand)
    {
      return MakeErrorResult(static_cast<int32>(CalculatorErrorCode::ComponentOutOfRange), fmt::format("Component index {} is out of range for array with {} components.", compIdx, numCompsOperand));
    }

    Result<float64> valueResult = evaluateSingleValue(dataStructure, rpn, operandIdx, spanStart, tupleIdx, compIdx, units);
    if(valueResult.invalid())
    {
      return ConvertResult(std::move(valueResult));
    }

    RpnItem scalarItem;
    scalarItem.type = RpnItem::Type::Scalar;
    scalarItem.scalarValue = valueResult.value();

    rpn.erase(rpn.begin() + static_cast<std::ptrdiff_t>(operandStart), rpn.begin() + static_cast<std::ptrdiff_t>(tceIndex) + 1);
    rpn.insert(rpn.begin() + static_cast<std::ptrdiff_t>(operandStart), scalarItem);
  }

  return {};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// getOperatorRegistry
// ---------------------------------------------------------------------------
const std::vector<OperatorDef>& nx::core::getOperatorRegistry()
{
  static const std::vector<OperatorDef> s_Registry = []() {
    std::vector<OperatorDef> reg;
    reg.reserve(23);

    // ---- Binary infix operators ----
    reg.push_back({"+", OperatorDef::BinaryInfix, 1, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return a + b; }});
    reg.push_back({"-", OperatorDef::BinaryInfix, 1, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return a - b; }});
    reg.push_back({"*", OperatorDef::BinaryInfix, 2, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return a * b; }});
    reg.push_back({"/", OperatorDef::BinaryInfix, 2, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return a / b; }});
    reg.push_back({"%", OperatorDef::BinaryInfix, 2, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return std::fmod(a, b); }});
    reg.push_back({"^", OperatorDef::BinaryInfix, 3, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return std::pow(a, b); }});

    // ---- Unary functions (1-arg) ----
    reg.push_back({"abs", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::None, [](double x) { return std::abs(x); }, nullptr});
    reg.push_back({"sqrt", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::None, [](double x) { return std::sqrt(x); }, nullptr});
    reg.push_back({"ceil", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::None, [](double x) { return std::ceil(x); }, nullptr});
    reg.push_back({"floor", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::None, [](double x) { return std::floor(x); }, nullptr});
    reg.push_back({"exp", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::None, [](double x) { return std::exp(x); }, nullptr});
    reg.push_back({"ln", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::None, [](double x) { return std::log(x); }, nullptr});
    reg.push_back({"log10", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::None, [](double x) { return std::log10(x); }, nullptr});

    // ---- Trig functions (1-arg, ForwardTrig) ----
    reg.push_back({"sin", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::ForwardTrig, [](double x) { return std::sin(x); }, nullptr});
    reg.push_back({"cos", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::ForwardTrig, [](double x) { return std::cos(x); }, nullptr});
    reg.push_back({"tan", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::ForwardTrig, [](double x) { return std::tan(x); }, nullptr});

    // ---- Inverse trig functions (1-arg, InverseTrig) ----
    reg.push_back({"asin", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::InverseTrig, [](double x) { return std::asin(x); }, nullptr});
    reg.push_back({"acos", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::InverseTrig, [](double x) { return std::acos(x); }, nullptr});
    reg.push_back({"atan", OperatorDef::Function, 5, 1, OperatorDef::Left, OperatorDef::InverseTrig, [](double x) { return std::atan(x); }, nullptr});

    // ---- Binary functions (2-arg) ----
    // NOTE: log (2-arg) must come AFTER log10 so that prefix matching during
    //       identifier resolution checks "log10" before "log".
    reg.push_back({"log", OperatorDef::Function, 5, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double base, double val) { return std::log(val) / std::log(base); }});
    reg.push_back({"root", OperatorDef::Function, 5, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double val, double n) { return std::pow(val, 1.0 / n); }});
    reg.push_back({"min", OperatorDef::Function, 5, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return std::min(a, b); }});
    reg.push_back({"max", OperatorDef::Function, 5, 2, OperatorDef::Left, OperatorDef::None, nullptr, [](double a, double b) { return std::max(a, b); }});

    return reg;
  }();

  return s_Registry;
}

// ---------------------------------------------------------------------------
// ArrayCalculatorParser
// ---------------------------------------------------------------------------
ArrayCalculatorParser::ArrayCalculatorParser(const DataStructure& dataStructure, const DataPath& selectedGroupPath, const std::string& infixEquation, const std::atomic_bool& shouldCancel)
: m_DataStructure(dataStructure)
, m_SelectedGroupPath(selectedGroupPath)
, m_InfixEquation(infixEquation)
, m_ShouldCancel(shouldCancel)
{
}

// ---------------------------------------------------------------------------
std::vector<Token> ArrayCalculatorParser::tokenize(const std::string& equation)
{
  std::vector<Token> tokens;
  const size_t len = equation.size();
  size_t i = 0;

  while(i < len)
  {
    const char c = equation[i];

    // 1. Skip whitespace
    if(std::isspace(static_cast<unsigned char>(c)))
    {
      ++i;
      continue;
    }

    // 2. Numbers: starts with digit, or dot followed by a digit
    if(std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < len && std::isdigit(static_cast<unsigned char>(equation[i + 1]))))
    {
      size_t start = i;
      bool hasDot = false;
      while(i < len && (std::isdigit(static_cast<unsigned char>(equation[i])) || equation[i] == '.'))
      {
        if(equation[i] == '.')
        {
          if(hasDot)
          {
            break;
          }
          hasDot = true;
        }
        ++i;
      }
      tokens.push_back({TokenType::Number, equation.substr(start, i - start), start});
      continue;
    }

    // 3. Identifiers: start with letter or underscore
    if(std::isalpha(static_cast<unsigned char>(c)) || c == '_')
    {
      size_t start = i;
      while(i < len && (std::isalnum(static_cast<unsigned char>(equation[i])) || equation[i] == '_'))
      {
        ++i;
      }
      tokens.push_back({TokenType::Identifier, equation.substr(start, i - start), start});
      continue;
    }

    // 4. Quoted strings
    if(c == '"')
    {
      size_t start = i;
      ++i; // skip opening quote
      size_t contentStart = i;
      while(i < len && equation[i] != '"')
      {
        ++i;
      }
      std::string content = equation.substr(contentStart, i - contentStart);
      if(i < len)
      {
        ++i; // skip closing quote
      }
      tokens.push_back({TokenType::QuotedString, content, start});
      continue;
    }

    // 5. Single-character operators
    TokenType opType;
    bool isOperator = true;
    switch(c)
    {
    case '+':
      opType = TokenType::Plus;
      break;
    case '-':
      opType = TokenType::Minus;
      break;
    case '*':
      opType = TokenType::Star;
      break;
    case '/':
      opType = TokenType::Slash;
      break;
    case '^':
      opType = TokenType::Caret;
      break;
    case '%':
      opType = TokenType::Percent;
      break;
    case '(':
      opType = TokenType::LParen;
      break;
    case ')':
      opType = TokenType::RParen;
      break;
    case '[':
      opType = TokenType::LBracket;
      break;
    case ']':
      opType = TokenType::RBracket;
      break;
    case ',':
      opType = TokenType::Comma;
      break;
    default:
      isOperator = false;
      break;
    }

    if(isOperator)
    {
      tokens.push_back({opType, std::string(1, c), i});
      ++i;
      continue;
    }

    // 6. Unknown characters: produce an Identifier token
    tokens.push_back({TokenType::Identifier, std::string(1, c), i});
    ++i;
  }

  return tokens;
}

// ---------------------------------------------------------------------------
// parse() -- the core parsing pipeline
// ---------------------------------------------------------------------------
Result<> ArrayCalculatorParser::parse()
{
  Result<> result;

  // === Step 1: Tokenize ===
  std::vector<Token> tokens = tokenize(m_InfixEquation);
  if(tokens.empty())
  {
    return MakeErrorResult(static_cast<int>(CalculatorErrorCode::EmptyEquation), "The infix expression is empty.");
  }

  // === Step 2: Multi-word identifier merging ===
  // Walk the token list. When consecutive Identifier tokens appear, try
  // merging them with spaces (greedy longest-first) and check if the
  // merged name matches an array name.
  {
    std::vector<Token> merged;
    merged.reserve(tokens.size());
    size_t i = 0;
    while(i < tokens.size())
    {
      if(tokens[i].type == TokenType::Identifier)
      {
        // Find the run of consecutive Identifier tokens
        size_t runStart = i;
        size_t runEnd = i + 1;
        while(runEnd < tokens.size() && tokens[runEnd].type == TokenType::Identifier)
        {
          ++runEnd;
        }
        size_t runLen = runEnd - runStart;

        if(runLen > 1)
        {
          // Greedy: try longest merge first
          bool foundMatch = false;
          for(size_t len = runLen; len >= 2; --len)
          {
            for(size_t start = runStart; start + len <= runEnd; ++start)
            {
              // Build the merged name
              std::string mergedName = tokens[start].text;
              for(size_t k = start + 1; k < start + len; ++k)
              {
                mergedName += " " + tokens[k].text;
              }

              // Check if this name matches an array
              bool found = false;
              if(!m_SelectedGroupPath.empty())
              {
                found = ContainsDataArrayName(m_DataStructure, m_SelectedGroupPath, mergedName);
              }
              if(!found)
              {
                auto paths = findArraysByName(m_DataStructure, mergedName);
                found = !paths.empty();
              }

              if(found)
              {
                // Add tokens before the match
                for(size_t k = runStart; k < start; ++k)
                {
                  merged.push_back(tokens[k]);
                }
                // Add the merged token
                merged.push_back({TokenType::Identifier, mergedName, tokens[start].position});
                // Add tokens after the match
                for(size_t k = start + len; k < runEnd; ++k)
                {
                  merged.push_back(tokens[k]);
                }
                i = runEnd;
                foundMatch = true;
                break;
              }
            }
            if(foundMatch)
            {
              break;
            }
          }
          if(!foundMatch)
          {
            // No merge found; copy all identifiers as-is
            for(size_t k = runStart; k < runEnd; ++k)
            {
              merged.push_back(tokens[k]);
            }
            i = runEnd;
          }
        }
        else
        {
          merged.push_back(tokens[i]);
          ++i;
        }
      }
      else
      {
        merged.push_back(tokens[i]);
        ++i;
      }
    }
    tokens = std::move(merged);
  }

  // === Steps 3+4: Identifier resolution, token conversion, and bracket indexing ===
  // These steps are combined so brackets can reference the array they follow.
  std::vector<ParsedItem> items;
  items.reserve(tokens.size());

  // Combined Step 3+4: resolve identifiers and handle brackets inline
  for(size_t i = 0; i < tokens.size(); ++i)
  {
    const Token& tok = tokens[i];

    // Check if this token starts a bracket expression [...]
    if(tok.type == TokenType::LBracket)
    {
      // Parse bracket contents: [Number] or [Number, Number]
      if(items.empty())
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::OrphanedComponent), "Index operator '[' is not paired with a valid array name or closing parenthesis.");
      }

      // Collect tokens until matching ']'
      std::vector<std::string> bracketNumbers;
      size_t j = i + 1;
      while(j < tokens.size() && tokens[j].type != TokenType::RBracket)
      {
        if(tokens[j].type == TokenType::Number)
        {
          bracketNumbers.push_back(tokens[j].text);
        }
        else if(tokens[j].type == TokenType::Comma)
        {
          // skip comma
        }
        else
        {
          return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidComponent), fmt::format("Invalid content inside bracket index: '{}'.", tokens[j].text));
        }
        ++j;
      }
      if(j >= tokens.size())
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::MismatchedParentheses), "Missing closing bracket ']'.");
      }
      // j now points to ']'

      ParsedItem& prevItem = items.back();

      if(prevItem.kind == ParsedItem::Kind::ArrayRef)
      {
        // Case A: Array[C] or Array[T, C]
        usize numComponents = 1;
        for(usize d : prevItem.arrayCompShape)
        {
          numComponents *= d;
        }
        usize numTuples = 1;
        for(usize d : prevItem.arrayTupleShape)
        {
          numTuples *= d;
        }

        if(bracketNumbers.size() == 1)
        {
          // [C]: component extraction
          usize compIdx = 0;
          try
          {
            auto parsed = std::stoull(bracketNumbers[0]);
            compIdx = static_cast<usize>(parsed);
          } catch(const std::exception&)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidComponent), fmt::format("Invalid component index '{}'.", bracketNumbers[0]));
          }

          if(compIdx >= numComponents)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::ComponentOutOfRange), fmt::format("Component index {} is out of range for array with {} components.", compIdx, numComponents));
          }

          // Emit a ComponentExtract after the ArrayRef
          ParsedItem ce;
          ce.kind = ParsedItem::Kind::ComponentExtract;
          ce.componentIndex = compIdx;
          items.push_back(ce);
        }
        else if(bracketNumbers.size() == 2)
        {
          // [T, C]: tuple+component extraction
          usize tupleIdx = 0;
          usize compIdx = 0;
          try
          {
            tupleIdx = static_cast<usize>(std::stoull(bracketNumbers[0]));
            compIdx = static_cast<usize>(std::stoull(bracketNumbers[1]));
          } catch(const std::exception&)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidComponent), fmt::format("Invalid tuple/component index in '[{}, {}]'.", bracketNumbers[0], bracketNumbers[1]));
          }

          if(tupleIdx >= numTuples)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::TupleOutOfRange), fmt::format("Tuple index {} is out of range for array with {} tuples.", tupleIdx, numTuples));
          }
          if(compIdx >= numComponents)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::ComponentOutOfRange), fmt::format("Component index {} is out of range for array with {} components.", compIdx, numComponents));
          }

          ParsedItem tce;
          tce.kind = ParsedItem::Kind::TupleComponentExtract;
          tce.tupleIndex = tupleIdx;
          tce.componentIndex = compIdx;
          items.push_back(tce);
        }
        else
        {
          return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidComponent), "Bracket index must contain 1 or 2 numbers (e.g. [C] or [T, C]).");
        }
      }
      else if(prevItem.kind == ParsedItem::Kind::RParen)
      {
        // Case B: )[C] or )[T, C] -- extraction on sub-expression result
        if(bracketNumbers.size() == 1)
        {
          // )[C]: component extraction
          usize compIdx = 0;
          try
          {
            compIdx = static_cast<usize>(std::stoull(bracketNumbers[0]));
          } catch(const std::exception&)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidComponent), fmt::format("Invalid component index '{}'.", bracketNumbers[0]));
          }

          ParsedItem ce;
          ce.kind = ParsedItem::Kind::ComponentExtract;
          ce.componentIndex = compIdx;
          items.push_back(ce);
        }
        else if(bracketNumbers.size() == 2)
        {
          // )[T, C]: tuple+component extraction (produces scalar)
          usize tupleIdx = 0;
          usize compIdx = 0;
          try
          {
            tupleIdx = static_cast<usize>(std::stoull(bracketNumbers[0]));
            compIdx = static_cast<usize>(std::stoull(bracketNumbers[1]));
          } catch(const std::exception&)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidComponent), fmt::format("Invalid tuple/component index in '[{}, {}]'.", bracketNumbers[0], bracketNumbers[1]));
          }

          ParsedItem tce;
          tce.kind = ParsedItem::Kind::TupleComponentExtract;
          tce.tupleIndex = tupleIdx;
          tce.componentIndex = compIdx;
          items.push_back(tce);
        }
        else
        {
          return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidComponent), "Sub-expression index must be [C] or [T, C].");
        }
      }
      else
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::OrphanedComponent), fmt::format("Index operator '{}' is not paired with a valid array name or closing parenthesis.", tok.text));
      }

      i = j; // skip past ']'
      continue;
    }

    // Normal token processing (same as step 3 above but now with brackets handled separately)
    switch(tok.type)
    {
    case TokenType::Number: {
      double numValue = 0.0;
      try
      {
        numValue = std::stod(tok.text);
      } catch(const std::exception&)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidEquation), fmt::format("Invalid numeric value '{}'.", tok.text));
      }

      ParsedItem pi;
      pi.kind = ParsedItem::Kind::Scalar;
      pi.scalarValue = numValue;
      items.push_back(pi);

      // Ambiguous name warning
      {
        bool arrayExists = false;
        if(!m_SelectedGroupPath.empty())
        {
          arrayExists = ContainsDataArrayName(m_DataStructure, m_SelectedGroupPath, tok.text);
        }
        if(!arrayExists)
        {
          arrayExists = !findArraysByName(m_DataStructure, tok.text).empty();
        }
        if(arrayExists)
        {
          result.warnings().push_back(Warning{static_cast<int>(CalculatorWarningCode::AmbiguousNameWarning),
                                              fmt::format("Item '{}' in the infix expression is the name of an array, but it is currently being used as a number."
                                                          "\nTo treat this item as an array name, please add double quotes around the item (i.e. \"{}\").",
                                                          tok.text, tok.text)});
        }
      }
      break;
    }

    case TokenType::Identifier: {
      const OperatorDef* opDef = findOperatorByToken(tok.text);
      if(opDef != nullptr)
      {
        {
          bool arrayExists = false;
          if(!m_SelectedGroupPath.empty())
          {
            arrayExists = ContainsDataArrayName(m_DataStructure, m_SelectedGroupPath, tok.text);
          }
          if(!arrayExists)
          {
            arrayExists = !findArraysByName(m_DataStructure, tok.text).empty();
          }
          if(arrayExists)
          {
            result.warnings().push_back(Warning{static_cast<int>(CalculatorWarningCode::AmbiguousNameWarning),
                                                fmt::format("Item '{}' in the infix expression is the name of an array, but it is currently being used as a mathematical operator."
                                                            "\nTo treat this item as an array name, please add double quotes around the item (i.e. \"{}\").",
                                                            tok.text, tok.text)});
          }
        }

        ParsedItem pi;
        pi.kind = ParsedItem::Kind::Operator;
        pi.op = opDef;
        items.push_back(pi);
      }
      else if(tok.text == "pi" || tok.text == "e")
      {
        float64 constValue = (tok.text == "pi") ? std::numbers::pi : std::numbers::e;

        ParsedItem pi;
        pi.kind = ParsedItem::Kind::Scalar;
        pi.scalarValue = constValue;
        items.push_back(pi);

        {
          bool arrayExists = false;
          if(!m_SelectedGroupPath.empty())
          {
            arrayExists = ContainsDataArrayName(m_DataStructure, m_SelectedGroupPath, tok.text);
          }
          if(!arrayExists)
          {
            arrayExists = !findArraysByName(m_DataStructure, tok.text).empty();
          }
          if(arrayExists)
          {
            result.warnings().push_back(Warning{static_cast<int>(CalculatorWarningCode::AmbiguousNameWarning),
                                                fmt::format("Item '{}' in the infix expression is the name of an array, but it is currently being used as a built-in constant."
                                                            "\nTo treat this item as an array name, please add double quotes around the item (i.e. \"{}\").",
                                                            tok.text, tok.text)});
          }
        }
      }
      else
      {
        // Try as array name
        if(!m_SelectedGroupPath.empty() && ContainsDataArrayName(m_DataStructure, m_SelectedGroupPath, tok.text))
        {
          DataPath arrayPath = m_SelectedGroupPath.createChildPath(tok.text);
          const auto* dataArray = m_DataStructure.getDataAs<IDataArray>(arrayPath);
          if(dataArray == nullptr)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::UnrecognizedItem), fmt::format("Could not access array '{}' in selected group.", tok.text));
          }

          ParsedItem pi;
          pi.kind = ParsedItem::Kind::ArrayRef;
          pi.arrayPath = arrayPath;
          pi.sourceDataType = dataArray->getDataType();
          pi.arrayTupleShape = dataArray->getTupleShape();
          pi.arrayCompShape = dataArray->getComponentShape();
          items.push_back(pi);
        }
        else
        {
          auto foundPaths = findArraysByName(m_DataStructure, tok.text);
          if(foundPaths.empty())
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::UnrecognizedItem), fmt::format("An unrecognized or invalid item '{}' was found in the chosen infix expression.", tok.text));
          }
          if(foundPaths.size() > 1)
          {
            std::string pathsList;
            for(const auto& p : foundPaths)
            {
              if(!pathsList.empty())
              {
                pathsList += ", ";
              }
              pathsList += p.toString();
            }
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::AmbiguousArrayName), fmt::format("Array name '{}' is ambiguous. Multiple arrays found: [{}]."
                                                                                                          "\nPlease use double quotes with the full path (e.g. \"Path/To/{}\") to disambiguate.",
                                                                                                          tok.text, pathsList, tok.text));
          }
          const auto* dataArray = m_DataStructure.getDataAs<IDataArray>(foundPaths[0]);
          if(dataArray == nullptr)
          {
            return MakeErrorResult(static_cast<int>(CalculatorErrorCode::UnrecognizedItem), fmt::format("Could not access array '{}'.", tok.text));
          }

          ParsedItem pi;
          pi.kind = ParsedItem::Kind::ArrayRef;
          pi.arrayPath = foundPaths[0];
          pi.sourceDataType = dataArray->getDataType();
          pi.arrayTupleShape = dataArray->getTupleShape();
          pi.arrayCompShape = dataArray->getComponentShape();
          items.push_back(pi);
        }
      }
      break;
    }

    case TokenType::QuotedString: {
      std::vector<std::string> pathComponents;
      {
        std::string component;
        for(char ch : tok.text)
        {
          if(ch == '/')
          {
            if(!component.empty())
            {
              pathComponents.push_back(component);
              component.clear();
            }
          }
          else
          {
            component += ch;
          }
        }
        if(!component.empty())
        {
          pathComponents.push_back(component);
        }
      }

      DataPath quotedPath(pathComponents);

      // If single component, try as child of selected group first
      if(pathComponents.size() == 1 && !m_SelectedGroupPath.empty())
      {
        DataPath childPath = m_SelectedGroupPath.createChildPath(pathComponents[0]);
        if(m_DataStructure.getDataAs<IDataArray>(childPath) != nullptr)
        {
          quotedPath = childPath;
        }
      }

      const auto* dataArray = m_DataStructure.getDataAs<IDataArray>(quotedPath);
      if(dataArray == nullptr)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidArrayName), fmt::format("The item '\"{}\"' is not a valid array path in the DataStructure.", tok.text));
      }

      ParsedItem pi;
      pi.kind = ParsedItem::Kind::ArrayRef;
      pi.arrayPath = quotedPath;
      pi.sourceDataType = dataArray->getDataType();
      pi.arrayTupleShape = dataArray->getTupleShape();
      pi.arrayCompShape = dataArray->getComponentShape();
      items.push_back(pi);
      break;
    }

    case TokenType::Plus:
    case TokenType::Minus:
    case TokenType::Star:
    case TokenType::Slash:
    case TokenType::Caret:
    case TokenType::Percent: {
      const OperatorDef* opDef = operatorDefForSymbolToken(tok.type);
      if(opDef == nullptr)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidSymbol), fmt::format("Unknown operator symbol '{}'.", tok.text));
      }

      // Check if the operator symbol is also the name of an array
      {
        bool arrayExists = false;
        if(!m_SelectedGroupPath.empty())
        {
          arrayExists = ContainsDataArrayName(m_DataStructure, m_SelectedGroupPath, tok.text);
        }
        if(!arrayExists)
        {
          arrayExists = !findArraysByName(m_DataStructure, tok.text).empty();
        }
        if(arrayExists)
        {
          result.warnings().push_back(Warning{static_cast<int>(CalculatorWarningCode::AmbiguousNameWarning),
                                              fmt::format("Item '{}' in the infix expression is the name of an array, but it is currently being used as a mathematical operator."
                                                          "\nTo treat this item as an array name, please add double quotes around the item (i.e. \"{}\").",
                                                          tok.text, tok.text)});
        }
      }

      ParsedItem pi;
      pi.kind = ParsedItem::Kind::Operator;
      pi.op = opDef;
      items.push_back(pi);
      break;
    }

    case TokenType::LParen: {
      ParsedItem pi;
      pi.kind = ParsedItem::Kind::LParen;
      items.push_back(pi);
      break;
    }

    case TokenType::RParen: {
      ParsedItem pi;
      pi.kind = ParsedItem::Kind::RParen;
      items.push_back(pi);
      break;
    }

    case TokenType::Comma: {
      ParsedItem pi;
      pi.kind = ParsedItem::Kind::Comma;
      items.push_back(pi);
      break;
    }

    case TokenType::LBracket:
    case TokenType::RBracket: {
      // Should not reach here since brackets are handled at the top of the loop
      return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InvalidEquation), "Unexpected bracket token encountered.");
    }

    } // end switch
  }

  // === Step 5: Minus sign disambiguation ===
  for(size_t i = 0; i < items.size(); ++i)
  {
    auto& item = items[i];
    if(item.kind != ParsedItem::Kind::Operator || item.op == nullptr)
    {
      continue;
    }
    if(item.op->token != "-")
    {
      continue;
    }

    // Determine if this is a unary negative
    bool isUnary = false;
    if(i == 0)
    {
      isUnary = true;
    }
    else
    {
      const auto& prev = items[i - 1];
      if(isBinaryOp(prev))
      {
        isUnary = true;
      }
      else if(prev.kind == ParsedItem::Kind::Operator && prev.op != nullptr && prev.op->kind == OperatorDef::UnaryPrefix)
      {
        isUnary = true;
      }
      else if(prev.kind == ParsedItem::Kind::LParen)
      {
        isUnary = true;
      }
      else if(prev.kind == ParsedItem::Kind::Comma)
      {
        isUnary = true;
      }
    }

    if(isUnary)
    {
      item.op = &getUnaryNegativeOp();
      item.isNegativePrefix = true;
    }
  }

  // === Step 6: WrapFunctionArguments ===
  wrapFunctionArguments(items);

  // === Step 7: Validation ===

  // 7a-1: Check for function/unary operators: opening/closing paren, argument count, empty args
  for(size_t i = 0; i < items.size(); ++i)
  {
    const auto& item = items[i];
    if(item.kind == ParsedItem::Kind::Operator && item.op != nullptr && item.op->kind == OperatorDef::Function)
    {
      // A function operator must be followed by LParen
      if(i + 1 >= items.size() || items[i + 1].kind != ParsedItem::Kind::LParen)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::OperatorNoOpeningParen), fmt::format("The operator/function '{}' does not have a valid opening parenthesis.", item.op->token));
      }

      // Find the matching RParen and count commas/values at depth 1
      int depth = 0;
      bool foundClose = false;
      size_t closeIdx = 0;
      int commaCount = 0;
      bool hasValueInside = false;
      for(size_t j = i + 1; j < items.size(); ++j)
      {
        if(items[j].kind == ParsedItem::Kind::LParen)
        {
          ++depth;
        }
        else if(items[j].kind == ParsedItem::Kind::RParen)
        {
          --depth;
          if(depth == 0)
          {
            foundClose = true;
            closeIdx = j;
            break;
          }
        }
        else if(items[j].kind == ParsedItem::Kind::Comma && depth == 1)
        {
          ++commaCount;
        }
        else if(depth >= 1 && (items[j].kind == ParsedItem::Kind::Scalar || items[j].kind == ParsedItem::Kind::ArrayRef || (items[j].kind == ParsedItem::Kind::Operator && items[j].op != nullptr)))
        {
          hasValueInside = true;
        }
      }
      if(!foundClose)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::OperatorNoClosingParen), fmt::format("The operator/function '{}' does not have a valid closing parenthesis.", item.op->token));
      }

      // Check for empty function call: func() with no values or commas inside
      if(!hasValueInside && commaCount == 0)
      {
        // For 2-arg functions with empty parens: NotEnoughArguments
        if(item.op->numArgs == 2)
        {
          return MakeErrorResult(static_cast<int>(CalculatorErrorCode::NotEnoughArguments),
                                 fmt::format("The function '{}' requires {} arguments, but none were provided.", item.op->token, item.op->numArgs));
        }
        // For 1-arg functions with empty parens: NoNumericArguments
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::NoNumericArguments), fmt::format("The function '{}' does not have any arguments that simplify down to a number.", item.op->token));
      }

      // Check for commas in the empty-value case: func(,) -- commas but no real values
      if(!hasValueInside && commaCount > 0)
      {
        if(item.op->numArgs == 1)
        {
          return MakeErrorResult(static_cast<int>(CalculatorErrorCode::TooManyArguments),
                                 fmt::format("The function '{}' requires {} argument, but more were provided.", item.op->token, item.op->numArgs));
        }
        // For 2-arg functions: NoNumericArguments (commas but no values)
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::NoNumericArguments), fmt::format("The function '{}' does not have any arguments that simplify down to a number.", item.op->token));
      }

      // Argument count: numArgs from OperatorDef, commaCount gives (numArgs-1)
      int providedArgs = commaCount + 1;
      if(item.op->numArgs == 1 && commaCount > 0)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::TooManyArguments),
                               fmt::format("The function '{}' requires {} argument, but {} were provided.", item.op->token, item.op->numArgs, providedArgs));
      }
      if(item.op->numArgs == 2 && commaCount < 1 && hasValueInside)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::NotEnoughArguments),
                               fmt::format("The function '{}' requires {} arguments, but only {} was provided.", item.op->token, item.op->numArgs, providedArgs));
      }
    }
  }

  // 7a-1b: Check for commas inside non-function parentheses (NoPrecedingUnaryOperator)
  for(size_t i = 0; i < items.size(); ++i)
  {
    if(items[i].kind == ParsedItem::Kind::Comma)
    {
      // Walk backwards to find the opening paren at the same depth, and check if preceded by a function
      int depth = 0;
      bool foundFunction = false;
      for(int j = static_cast<int>(i) - 1; j >= 0; --j)
      {
        if(items[j].kind == ParsedItem::Kind::RParen)
        {
          ++depth;
        }
        else if(items[j].kind == ParsedItem::Kind::LParen)
        {
          if(depth == 0)
          {
            // Found the opening paren; check if preceded by a function
            if(j > 0 && items[j - 1].kind == ParsedItem::Kind::Operator && items[j - 1].op != nullptr && items[j - 1].op->kind == OperatorDef::Function)
            {
              foundFunction = true;
            }
            break;
          }
          --depth;
        }
      }
      if(!foundFunction)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::NoPrecedingUnaryOperator), "A comma was found in parentheses without a preceding function operator.");
      }
    }
  }

  // 7a-2: Check for binary operators missing left or right operands
  for(size_t i = 0; i < items.size(); ++i)
  {
    const auto& item = items[i];
    if(!isBinaryOp(item))
    {
      continue;
    }
    // Check left: the item before must be a value or RParen (something that produces a value)
    bool hasLeft = false;
    if(i > 0)
    {
      const auto& prev = items[i - 1];
      if(prev.kind == ParsedItem::Kind::Scalar || prev.kind == ParsedItem::Kind::ArrayRef || prev.kind == ParsedItem::Kind::RParen || prev.kind == ParsedItem::Kind::ComponentExtract ||
         prev.kind == ParsedItem::Kind::TupleComponentExtract)
      {
        hasLeft = true;
      }
    }
    if(!hasLeft)
    {
      return MakeErrorResult(static_cast<int>(CalculatorErrorCode::OperatorNoLeftValue), fmt::format("The binary operator '{}' does not have a valid left-hand value.", item.op->token));
    }
    // Check right: the item after must be a value, LParen, or unary operator (something that produces a value)
    bool hasRight = false;
    if(i + 1 < items.size())
    {
      const auto& next = items[i + 1];
      if(next.kind == ParsedItem::Kind::Scalar || next.kind == ParsedItem::Kind::ArrayRef || next.kind == ParsedItem::Kind::LParen)
      {
        hasRight = true;
      }
      else if(next.kind == ParsedItem::Kind::Operator && next.op != nullptr)
      {
        hasRight = true; // Could be a unary prefix or function
      }
    }
    if(!hasRight)
    {
      return MakeErrorResult(static_cast<int>(CalculatorErrorCode::OperatorNoRightValue), fmt::format("The binary operator '{}' does not have a valid right-hand value.", item.op->token));
    }
  }

  // 7a-3: Check for unary negative with no right operand
  for(size_t i = 0; i < items.size(); ++i)
  {
    const auto& item = items[i];
    if(item.isNegativePrefix)
    {
      bool hasRight = false;
      if(i + 1 < items.size())
      {
        const auto& next = items[i + 1];
        if(next.kind == ParsedItem::Kind::Scalar || next.kind == ParsedItem::Kind::ArrayRef || next.kind == ParsedItem::Kind::LParen)
        {
          hasRight = true;
        }
        else if(next.kind == ParsedItem::Kind::Operator && next.op != nullptr)
        {
          hasRight = true; // e.g. -sin(...)
        }
      }
      if(!hasRight)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::OperatorNoRightValue), "The unary negative operator does not have a valid right-hand value.");
      }
    }
  }

  // 7a-4: Check matched parentheses (generic, after operator-specific checks)
  {
    int parenDepth = 0;
    for(const auto& item : items)
    {
      if(item.kind == ParsedItem::Kind::LParen)
      {
        ++parenDepth;
      }
      else if(item.kind == ParsedItem::Kind::RParen)
      {
        --parenDepth;
      }
      if(parenDepth < 0)
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::MismatchedParentheses),
                               fmt::format("One or more parentheses are mismatched in the chosen infix expression '{}'.", m_InfixEquation));
      }
    }
    if(parenDepth != 0)
    {
      return MakeErrorResult(static_cast<int>(CalculatorErrorCode::MismatchedParentheses), fmt::format("One or more parentheses are mismatched in the chosen infix expression '{}'.", m_InfixEquation));
    }
  }

  // 7b: Collect array-type values and verify consistent tuple/component info
  std::vector<usize> arrayTupleShape;
  std::vector<usize> arrayCompShape;
  usize arrayNumTuples = 0;
  bool hasArray = false;
  bool hasNumericValue = false;
  bool tupleShapesMatch = true;

  for(size_t vi = 0; vi < items.size(); ++vi)
  {
    const auto& item = items[vi];
    if(item.kind == ParsedItem::Kind::Scalar || item.kind == ParsedItem::Kind::ArrayRef)
    {
      hasNumericValue = true;
    }
    if(item.kind == ParsedItem::Kind::ArrayRef)
    {
      std::vector<usize> ts = item.arrayTupleShape;
      std::vector<usize> cs = item.arrayCompShape;

      // If this ArrayRef is immediately followed by ComponentExtract or
      // TupleComponentExtract, adjust the effective shape accordingly.
      if(vi + 1 < items.size() && items[vi + 1].kind == ParsedItem::Kind::ComponentExtract)
      {
        cs = {1};
      }
      else if(vi + 1 < items.size() && items[vi + 1].kind == ParsedItem::Kind::TupleComponentExtract)
      {
        // TupleComponentExtract reduces to a scalar — skip this array from
        // shape consistency checks entirely (it won't contribute shape).
        continue;
      }

      usize nt = 1;
      for(usize d : ts)
      {
        nt *= d;
      }

      if(hasArray)
      {
        if(!arrayCompShape.empty() && arrayCompShape != cs)
        {
          return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InconsistentCompDims), "Attribute Array symbols in the infix expression have mismatching component dimensions.");
        }
        if(arrayNumTuples != 0 && nt != arrayNumTuples)
        {
          return MakeErrorResult(static_cast<int>(CalculatorErrorCode::InconsistentTuples), "Attribute Array symbols in the infix expression have mismatching number of tuples.");
        }
        if(!arrayTupleShape.empty() && arrayTupleShape != ts)
        {
          tupleShapesMatch = false;
        }
      }

      hasArray = true;
      arrayTupleShape = ts;
      arrayCompShape = cs;
      arrayNumTuples = nt;
    }
  }

  // 7c: Ensure at least one numeric argument exists
  if(!hasNumericValue)
  {
    return MakeErrorResult(static_cast<int>(CalculatorErrorCode::NoNumericArguments), "The expression does not have any arguments that simplify down to a number.");
  }

  // Check if there is a ComponentExtract or TupleComponentExtract item in the parsed list.
  // ComponentExtract produces a single-component array.
  // TupleComponentExtract produces a scalar (single tuple, single component).
  bool hasComponentExtract = false;
  bool hasTupleComponentExtract = false;
  for(const auto& item : items)
  {
    if(item.kind == ParsedItem::Kind::ComponentExtract)
    {
      hasComponentExtract = true;
    }
    if(item.kind == ParsedItem::Kind::TupleComponentExtract)
    {
      hasTupleComponentExtract = true;
    }
  }

  // Store the parsed shape info for use by parseAndValidate()
  if(hasTupleComponentExtract)
  {
    // TupleComponentExtract produces a scalar
    m_ParsedTupleShape = {1};
    m_ParsedComponentShape = {1};
  }
  else if(hasArray)
  {
    if(tupleShapesMatch)
    {
      m_ParsedTupleShape = arrayTupleShape;
    }
    else
    {
      m_ParsedTupleShape = {arrayNumTuples};
    }
    m_ParsedComponentShape = hasComponentExtract ? std::vector<usize>{1} : arrayCompShape;
  }
  else
  {
    // All scalars: output is {1} tuples, {1} components
    m_ParsedTupleShape = {1};
    m_ParsedComponentShape = {1};
  }

  // === Convert ParsedItems to RPN using shunting-yard ===
  m_RpnItems.clear();
  std::vector<ParsedItem> opStack;

  for(const auto& item : items)
  {
    switch(item.kind)
    {
    case ParsedItem::Kind::Scalar: {
      RpnItem rpn;
      rpn.type = RpnItem::Type::Scalar;
      rpn.scalarValue = item.scalarValue;
      m_RpnItems.push_back(rpn);
      break;
    }

    case ParsedItem::Kind::ArrayRef: {
      RpnItem rpn;
      rpn.type = RpnItem::Type::ArrayRef;
      rpn.arrayPath = item.arrayPath;
      rpn.sourceDataType = item.sourceDataType;
      m_RpnItems.push_back(rpn);
      break;
    }

    case ParsedItem::Kind::LParen: {
      opStack.push_back(item);
      break;
    }

    case ParsedItem::Kind::RParen: {
      // Pop operators to output until we find the matching LParen
      while(!opStack.empty() && opStack.back().kind != ParsedItem::Kind::LParen)
      {
        const auto& top = opStack.back();
        RpnItem rpn;
        rpn.type = RpnItem::Type::Operator;
        rpn.op = top.op;
        m_RpnItems.push_back(rpn);
        opStack.pop_back();
      }
      if(opStack.empty())
      {
        return MakeErrorResult(static_cast<int>(CalculatorErrorCode::MismatchedParentheses),
                               fmt::format("One or more parentheses are mismatched in the chosen infix expression '{}'.", m_InfixEquation));
      }
      // Discard the LParen
      opStack.pop_back();
      break;
    }

    case ParsedItem::Kind::Comma: {
      // Pop operators to output until we find the LParen (but don't discard it)
      while(!opStack.empty() && opStack.back().kind != ParsedItem::Kind::LParen)
      {
        const auto& top = opStack.back();
        RpnItem rpn;
        rpn.type = RpnItem::Type::Operator;
        rpn.op = top.op;
        m_RpnItems.push_back(rpn);
        opStack.pop_back();
      }
      break;
    }

    case ParsedItem::Kind::Operator: {
      const OperatorDef* incomingOp = item.isNegativePrefix ? &getUnaryNegativeOp() : item.op;
      int incomingPrec = incomingOp->precedence;
      bool isLeftAssoc = (incomingOp->associativity == OperatorDef::Left);

      while(!opStack.empty() && opStack.back().kind == ParsedItem::Kind::Operator)
      {
        const auto& topItem = opStack.back();
        const OperatorDef* topOp = topItem.isNegativePrefix ? &getUnaryNegativeOp() : topItem.op;
        int topPrec = topOp->precedence;

        if(topPrec > incomingPrec || (topPrec == incomingPrec && isLeftAssoc))
        {
          RpnItem rpn;
          rpn.type = RpnItem::Type::Operator;
          rpn.op = topOp;
          m_RpnItems.push_back(rpn);
          opStack.pop_back();
        }
        else
        {
          break;
        }
      }

      opStack.push_back(item);
      break;
    }

    case ParsedItem::Kind::ComponentExtract: {
      RpnItem rpn;
      rpn.type = RpnItem::Type::ComponentExtract;
      rpn.componentIndex = item.componentIndex;
      m_RpnItems.push_back(rpn);
      break;
    }

    case ParsedItem::Kind::TupleComponentExtract: {
      RpnItem rpn;
      rpn.type = RpnItem::Type::TupleComponentExtract;
      rpn.tupleIndex = item.tupleIndex;
      rpn.componentIndex = item.componentIndex;
      m_RpnItems.push_back(rpn);
      break;
    }

    } // end switch
  }

  // Pop remaining operators from the stack
  while(!opStack.empty())
  {
    const auto& top = opStack.back();
    if(top.kind == ParsedItem::Kind::LParen)
    {
      return MakeErrorResult(static_cast<int>(CalculatorErrorCode::MismatchedParentheses), fmt::format("One or more parentheses are mismatched in the chosen infix expression '{}'.", m_InfixEquation));
    }
    const OperatorDef* topOp = top.isNegativePrefix ? &getUnaryNegativeOp() : top.op;
    RpnItem rpn;
    rpn.type = RpnItem::Type::Operator;
    rpn.op = topOp;
    m_RpnItems.push_back(rpn);
    opStack.pop_back();
  }

  return result;
}

// ---------------------------------------------------------------------------
Result<> ArrayCalculatorParser::parseAndValidate(std::vector<usize>& outTupleShape, std::vector<usize>& outComponentShape)
{
  Result<> parseResult = parse();
  if(parseResult.invalid())
  {
    return parseResult;
  }

  outTupleShape = m_ParsedTupleShape;
  outComponentShape = m_ParsedComponentShape;

  return parseResult;
}

// ---------------------------------------------------------------------------
Result<> ArrayCalculatorParser::evaluateInto(DataStructure& dataStructure, const DataPath& outputPath, NumericType scalarType, CalculatorParameter::AngleUnits units)
{
  // 1. Parse (populates m_RpnItems via shunting-yard)
  Result<> parseResult = parse();
  if(parseResult.invalid())
  {
    return parseResult;
  }

  const DataType outputDataType = ConvertNumericTypeToDataType(scalarType);

  // 2. Reduction pre-pass: collapse every Array[T, C] / (expr)[T, C] tuple+component extraction to
  // a cached float64 Scalar item. Works on a local copy since m_RpnItems is repopulated from
  // scratch by parse() on every call.
  std::vector<RpnItem> rpn = m_RpnItems;
  Result<> reduceResult = resolveTupleComponentExtracts(m_DataStructure, rpn, units);
  if(reduceResult.invalid())
  {
    return reduceResult;
  }

  // 3. Determine whether the (now TupleComponentExtract-free) expression collapses to a single
  // broadcastable scalar -- either because it was nothing but constants/operators to begin with
  // (e.g. "12 + 6"), or because the entire expression WAS one extraction that the pre-pass above
  // just resolved. Either way this is an O(expression length) computation, not O(data size).
  Result<ShapeSimResult> shapeResult = simulateShapes(m_DataStructure, rpn, 0, rpn.size() - 1);
  if(shapeResult.invalid())
  {
    return ConvertResult(std::move(shapeResult));
  }
  const ShapeSimResult& shapeInfo = shapeResult.value();

  if(shapeInfo.finalShape.isScalar)
  {
    const std::vector<usize> spanStart = computeSpanStarts(rpn);
    Result<float64> scalarResult = evaluateSingleValue(m_DataStructure, rpn, rpn.size() - 1, spanStart, 0, 0, units);
    if(scalarResult.invalid())
    {
      return ConvertResult(std::move(scalarResult));
    }
    ExecuteDataFunction(FillScalarResultFunctor{}, outputDataType, dataStructure, outputPath, scalarResult.value());
    return parseResult;
  }

  // 4. Main streaming pass: the expression is array-valued. Walk it once per bounded tuple-chunk,
  // writing each chunk's result directly into the output array. No buffer here scales with the
  // input/output tuple count -- every per-node buffer is capped at tuplesPerChunk * maxComponents
  // (bounded by chunk size and expression complexity) and is reused across every chunk.
  auto& outputArray = dataStructure.getDataRefAs<IDataArray>(outputPath);
  const usize outputNumTuples = outputArray.getNumberOfTuples();
  const usize outputNumComps = outputArray.getNumberOfComponents();
  const usize tuplesPerChunk = std::max<usize>(1, k_ChunkSize / std::max<usize>(1, outputNumComps));

  // Resolve every ArrayRef's source array once, up front, rather than re-resolving its DataPath on
  // every chunk iteration -- the same handful of arrays are read repeatedly, once per chunk.
  std::vector<const IDataArray*> resolvedArrays(rpn.size(), nullptr);
  for(usize i = 0; i < rpn.size(); i++)
  {
    if(rpn[i].type == RpnItem::Type::ArrayRef)
    {
      resolvedArrays[i] = m_DataStructure.getDataAs<IDataArray>(rpn[i].arrayPath);
      if(resolvedArrays[i] == nullptr)
      {
        return MakeErrorResult(static_cast<int32>(CalculatorErrorCode::InvalidEquation),
                               fmt::format("Internal error: array '{}' could not be resolved during evaluation.", rpn[i].arrayPath.toString()));
      }
    }
  }

  // Fixed-size evaluation stack: entries are reused in place for every RPN item of every chunk, so
  // no per-chunk or per-item heap churn occurs beyond the occasional resize() that keeps a buffer's
  // logical size in step with its node's current component count (capacity, reserved once below,
  // never grows past tuplesPerChunk * maxComponents).
  struct ChunkStackEntry
  {
    bool isScalar = false;
    float64 scalarValue = 0.0;
    usize numComponents = 1;
    std::vector<float64> buffer;
  };
  std::vector<ChunkStackEntry> stack(shapeInfo.maxStackDepth);
  for(auto& entry : stack)
  {
    entry.buffer.reserve(tuplesPerChunk * shapeInfo.maxComponents);
  }
  std::vector<float64> outWriteBuf;
  outWriteBuf.reserve(tuplesPerChunk * outputNumComps);

  for(usize tupleStart = 0; tupleStart < outputNumTuples; tupleStart += tuplesPerChunk)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    const usize tupleCount = std::min(tuplesPerChunk, outputNumTuples - tupleStart);
    usize depth = 0;

    for(usize rpnIdx = 0; rpnIdx < rpn.size(); rpnIdx++)
    {
      const RpnItem& item = rpn[rpnIdx];
      switch(item.type)
      {
      case RpnItem::Type::Scalar: {
        stack[depth].isScalar = true;
        stack[depth].scalarValue = item.scalarValue;
        depth++;
        break;
      }

      case RpnItem::Type::ArrayRef: {
        ChunkStackEntry& entry = stack[depth];
        const IDataArray* sourceArray = resolvedArrays[rpnIdx];
        const usize numComps = sourceArray->getNumberOfComponents();
        const usize count = tupleCount * numComps;
        entry.isScalar = false;
        entry.numComponents = numComps;
        entry.buffer.resize(count);
        if(item.sourceDataType == DataType::float64)
        {
          const auto& typedArray = dynamic_cast<const Float64Array&>(*sourceArray);
          typedArray.getDataStoreRef().copyIntoBuffer(tupleStart * numComps, nonstd::span<float64>(entry.buffer.data(), count));
        }
        else
        {
          ExecuteDataFunction(ReadChunkToFloat64Functor{}, item.sourceDataType, *sourceArray, tupleStart * numComps, nonstd::span<float64>(entry.buffer.data(), count));
        }
        depth++;
        break;
      }

      case RpnItem::Type::Operator: {
        const OperatorDef* op = item.op;
        if(op->numArgs == 1)
        {
          ChunkStackEntry& operand = stack[depth - 1];
          if(operand.isScalar)
          {
            float64 val = operand.scalarValue;
            if(op->trigMode == OperatorDef::ForwardTrig && units == CalculatorParameter::AngleUnits::Degrees)
            {
              val = val * (std::numbers::pi / 180.0);
            }
            float64 res = op->unaryOp(val);
            if(op->trigMode == OperatorDef::InverseTrig && units == CalculatorParameter::AngleUnits::Degrees)
            {
              res = res * (180.0 / std::numbers::pi);
            }
            operand.scalarValue = res;
          }
          else
          {
            const usize count = tupleCount * operand.numComponents;
            for(usize i = 0; i < count; i++)
            {
              float64 val = operand.buffer[i];
              if(op->trigMode == OperatorDef::ForwardTrig && units == CalculatorParameter::AngleUnits::Degrees)
              {
                val = val * (std::numbers::pi / 180.0);
              }
              float64 res = op->unaryOp(val);
              if(op->trigMode == OperatorDef::InverseTrig && units == CalculatorParameter::AngleUnits::Degrees)
              {
                res = res * (180.0 / std::numbers::pi);
              }
              operand.buffer[i] = res;
            }
          }
          // Net effect: pop 1, push 1 -- the result stays in the same slot, depth unchanged.
        }
        else
        {
          ChunkStackEntry& right = stack[depth - 1];
          ChunkStackEntry& left = stack[depth - 2];
          depth--; // pop right; the result replaces left, which becomes the new top of stack

          if(left.isScalar && right.isScalar)
          {
            left.scalarValue = op->binaryOp(left.scalarValue, right.scalarValue);
          }
          else
          {
            const bool leftWasScalar = left.isScalar;
            const bool rightWasScalar = right.isScalar;
            const float64 leftScalarVal = left.scalarValue;
            const float64 rightScalarVal = right.scalarValue;
            const usize numComps = leftWasScalar ? right.numComponents : left.numComponents;
            const usize count = tupleCount * numComps;

            left.buffer.resize(count);
            for(usize i = 0; i < count; i++)
            {
              const float64 lv = leftWasScalar ? leftScalarVal : left.buffer[i];
              const float64 rv = rightWasScalar ? rightScalarVal : right.buffer[i];
              left.buffer[i] = op->binaryOp(lv, rv);
            }
            left.isScalar = false;
            left.numComponents = numComps;
          }
        }
        break;
      }

      case RpnItem::Type::ComponentExtract: {
        ChunkStackEntry& operand = stack[depth - 1];
        const usize compIdx = item.componentIndex;
        if(!operand.isScalar)
        {
          const usize numComps = operand.numComponents;
          for(usize t = 0; t < tupleCount; t++)
          {
            operand.buffer[t] = operand.buffer[t * numComps + compIdx];
          }
          operand.buffer.resize(tupleCount);
          operand.numComponents = 1;
        }
        // If the operand is already an inherent scalar, extracting its only valid component (0)
        // is a no-op: the value and scalar-ness are unchanged.
        break;
      }

      case RpnItem::Type::TupleComponentExtract:
        // Unreachable: every TupleComponentExtract item was resolved to a Scalar in step 2 above.
        break;
      }
    }

    // depth == 1 here; stack[0] holds this chunk's fully-evaluated result.
    const ChunkStackEntry& result = stack[0];
    outWriteBuf.resize(tupleCount * outputNumComps);
    if(result.isScalar)
    {
      std::fill(outWriteBuf.begin(), outWriteBuf.end(), result.scalarValue);
    }
    else
    {
      std::copy(result.buffer.begin(), result.buffer.begin() + static_cast<std::ptrdiff_t>(tupleCount * outputNumComps), outWriteBuf.begin());
    }

    if(outputDataType == DataType::float64)
    {
      auto& outputStore = dataStructure.getDataRefAs<Float64Array>(outputPath).getDataStoreRef();
      outputStore.copyFromBuffer(tupleStart * outputNumComps, nonstd::span<const float64>(outWriteBuf.data(), tupleCount * outputNumComps));
    }
    else
    {
      ExecuteDataFunction(WriteChunkFromFloat64Functor{}, outputDataType, dataStructure, outputPath, tupleStart * outputNumComps,
                          nonstd::span<const float64>(outWriteBuf.data(), tupleCount * outputNumComps));
    }
  }

  return parseResult;
}

// ---------------------------------------------------------------------------
// ArrayCalculator
// ---------------------------------------------------------------------------
ArrayCalculator::ArrayCalculator(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ArrayCalculatorInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// ---------------------------------------------------------------------------
ArrayCalculator::~ArrayCalculator() noexcept = default;

// ---------------------------------------------------------------------------
const std::atomic_bool& ArrayCalculator::getCancel()
{
  return m_ShouldCancel;
}

// ---------------------------------------------------------------------------
Result<> ArrayCalculator::operator()()
{
  ArrayCalculatorParser parser(m_DataStructure, m_InputValues->SelectedGroup, m_InputValues->InfixEquation, m_ShouldCancel);
  return parser.evaluateInto(m_DataStructure, m_InputValues->CalculatedArray, m_InputValues->ScalarType, m_InputValues->Units);
}
