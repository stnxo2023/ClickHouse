#include <IO/ReadHelpers.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/Context.h>
#include <Parsers/TokenIterator.h>
#include <Processors/Formats/Impl/ValuesBlockInputFormat.h>
#include <Formats/FormatFactory.h>
#include <Formats/EscapingRuleUtils.h>
#include <Core/Block.h>
#include <base/find_symbols.h>
#include <Common/typeid_cast.h>
#include <Common/checkStackSize.h>
#include <Common/logger_useful.h>
#include <Core/Settings.h>
#include <Parsers/ASTLiteral.h>
#include <DataTypes/Serializations/SerializationNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/ObjectUtils.h>


namespace DB
{
namespace Setting
{
    extern const SettingsUInt64 max_parser_backtracks;
    extern const SettingsUInt64 max_parser_depth;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int SYNTAX_ERROR;
    extern const int TYPE_MISMATCH;
    extern const int SUPPORT_IS_DISABLED;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int CANNOT_READ_ALL_DATA;
}


ValuesBlockInputFormat::ValuesBlockInputFormat(
    ReadBuffer & in_,
    SharedHeader header_,
    const RowInputFormatParams & params_,
    const FormatSettings & format_settings_)
    : ValuesBlockInputFormat(std::make_unique<PeekableReadBuffer>(in_), header_, params_, format_settings_)
{
}

ValuesBlockInputFormat::ValuesBlockInputFormat(
    std::unique_ptr<PeekableReadBuffer> buf_,
    SharedHeader header_,
    const RowInputFormatParams & params_,
    const FormatSettings & format_settings_)
    : IInputFormat(header_, buf_.get()), buf(std::move(buf_)),
        params(params_), format_settings(format_settings_), num_columns(header_->columns()),
        parser_type_for_column(num_columns, ParserType::Streaming),
        attempts_to_deduce_template(num_columns), attempts_to_deduce_template_cached(num_columns),
        rows_parsed_using_template(num_columns), templates(num_columns), types(header_->getDataTypes()), serializations(header_->getSerializations())
    , block_missing_values(getPort().getHeader().columns())
{
}

/// Can be used in fileSegmentationEngine for parallel parsing of Values
bool ValuesBlockInputFormat::skipToNextRow(ReadBuffer * buf, size_t min_chunk_bytes, int balance)
{
    skipWhitespaceIfAny(*buf);
    if (buf->eof() || *buf->position() == ';')
        return false;
    bool quoted = false;

    size_t chunk_begin_buf_count = buf->count();
    while (!buf->eof() && (balance || buf->count() - chunk_begin_buf_count < min_chunk_bytes))
    {
        buf->position() = find_first_symbols<'\\', '\'', ')', '('>(buf->position(), buf->buffer().end());
        if (buf->position() == buf->buffer().end())
            continue;
        if (*buf->position() == '\\')
        {
            ++buf->position();
            if (!buf->eof())
                ++buf->position();
        }
        else if (*buf->position() == '\'')
        {
            quoted ^= true;
            ++buf->position();
        }
        else if (*buf->position() == ')')
        {
            ++buf->position();
            if (!quoted)
                --balance;
        }
        else if (*buf->position() == '(')
        {
            ++buf->position();
            if (!quoted)
                ++balance;
        }
    }

    if (!buf->eof() && *buf->position() == ',')
        ++buf->position();
    return true;
}

Chunk ValuesBlockInputFormat::read()
{
    if (total_rows == 0)
        readPrefix();

    const Block & header = getPort().getHeader();
    MutableColumns columns = header.cloneEmptyColumns();
    block_missing_values.clear();
    size_t chunk_start = getDataOffsetMaybeCompressed(*buf);

    size_t rows_in_block = 0;
    for (; rows_in_block < params.max_block_size; ++rows_in_block)
    {
        try
        {
            skipWhitespaceIfAny(*buf);
            if (buf->eof() || *buf->position() == ';')
                break;
            if (need_only_count)
                skipToNextRow(buf.get(), 1, 0);
            else
                readRow(columns, rows_in_block);
        }
        catch (Exception & e)
        {
            if (isParseError(e.code()))
                e.addMessage(" at row " + std::to_string(total_rows));
            throw;
        }
    }

    approx_bytes_read_for_chunk = getDataOffsetMaybeCompressed(*buf) - chunk_start;

    if (need_only_count)
    {
        if (!rows_in_block)
        {
            readSuffix();
            return {};
        }

        total_rows += rows_in_block;
        return getChunkForCount(rows_in_block);
    }

    /// Evaluate expressions, which were parsed using templates, if any
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (!templates[i] || !templates[i]->rowsCount())
            continue;

        const auto & expected_type = header.getByPosition(i).type;
        if (columns[i]->empty())
            columns[i] = IColumn::mutate(templates[i]->evaluateAll(block_missing_values, i, expected_type));
        else
        {
            ColumnPtr evaluated = templates[i]->evaluateAll(block_missing_values, i, expected_type, columns[i]->size());
            columns[i]->insertRangeFrom(*evaluated, 0, evaluated->size());
        }
    }

    if (columns.empty() || columns[0]->empty())
    {
        readSuffix();
        return {};
    }

    for (const auto & column : columns)
        column->finalize();

    size_t rows = columns[0]->size();
    return Chunk{std::move(columns), rows};
}

/// We need continuous memory containing the expression to use Lexer
/// Note that this is both reading and tokenizing until the end of the row
/// This is doing unnecessary work if the rest of the columns can be read with tryReadValue (which doesn't require tokens)
/// and it's more efficient if they don't (as everything is already tokenized)
void ValuesBlockInputFormat::readUntilTheEndOfRowAndReTokenize(size_t current_column_idx)
{
    if (tokens && token_iterator &&
        /// Make sure the underlying memory hasn't changed because of next() calls in the buffer
        ((*token_iterator)->begin >= buf->buffer().begin() && (*token_iterator)->begin <= buf->buffer().end()))
    {
        while ((*token_iterator)->begin < buf->position() && !(*token_iterator)->isError() && !(*token_iterator)->isEnd())
            ++(*token_iterator);

        if (!(*token_iterator)->isError() && !(*token_iterator)->isEnd())
            return;
    }

    skipToNextRow(buf.get(), 0, 1);
    buf->makeContinuousMemoryFromCheckpointToPos();
    auto * row_end = buf->position();
    buf->rollbackToCheckpoint();
    tokens.emplace(buf->position(), row_end);
    token_iterator.emplace(
        *tokens,
        static_cast<unsigned>(context->getSettingsRef()[Setting::max_parser_depth]),
        static_cast<unsigned>(context->getSettingsRef()[Setting::max_parser_backtracks]));
    auto const & first = (*token_iterator).get();
    if (first.isError() || first.isEnd())
    {
        const Block & header = getPort().getHeader();
        const IDataType & type = *header.getByPosition(current_column_idx).type;
        throw Exception(
            ErrorCodes::SYNTAX_ERROR,
            "Cannot parse expression of type {} here: {}",
            type.getName(),
            std::string_view(buf->position(), std::min(SHOW_CHARS_ON_SYNTAX_ERROR, buf->buffer().end() - buf->position())));
    }
}

void ValuesBlockInputFormat::readRow(MutableColumns & columns, size_t row_num)
{
    tokens.reset();
    token_iterator.reset();
    assertChar('(', *buf);

    for (size_t column_idx = 0; column_idx < num_columns; ++column_idx)
    {
        skipWhitespaceIfAny(*buf);
        PeekableReadBufferCheckpoint checkpoint{*buf};
        bool read = false;

        /// Parse value using fast streaming parser for literals and slow SQL parser for expressions.
        /// If there is SQL expression in some row, template of this expression will be deduced,
        /// so it makes possible to parse the following rows much faster
        /// if expressions in the following rows have the same structure
        if (parser_type_for_column[column_idx] == ParserType::Streaming)
            read = tryReadValue(*columns[column_idx], column_idx);
        else if (parser_type_for_column[column_idx] == ParserType::BatchTemplate)
            read = tryParseExpressionUsingTemplate(columns[column_idx], column_idx);
        else
            read = parseExpression(*columns[column_idx], column_idx);

        if (!read)
            block_missing_values.setBit(column_idx, row_num);
        /// If read is true, value still may be missing. Bit mask for these values will be copied from ConstantExpressionTemplate later.
    }

    skipWhitespaceIfAny(*buf);
    if (!buf->eof() && *buf->position() == ',')
        ++buf->position();

    ++total_rows;
}

bool ValuesBlockInputFormat::tryParseExpressionUsingTemplate(MutableColumnPtr & column, size_t column_idx)
{
    readUntilTheEndOfRowAndReTokenize(column_idx);
    IParser::Pos start = *token_iterator;

    /// Try to parse expression using template if one was successfully deduced while parsing the first row
    const auto & settings = context->getSettingsRef();
    if (templates[column_idx]->parseExpression(*buf, *token_iterator, format_settings, settings))
    {
        ++rows_parsed_using_template[column_idx];
        return true;
    }

    const auto & header = getPort().getHeader();
    const auto & expected_type = header.getByPosition(column_idx).type;

    /// Expression in the current row is not match template deduced on the first row.
    /// Evaluate expressions, which were parsed using this template.
    if (column->empty())
        column = IColumn::mutate(templates[column_idx]->evaluateAll(block_missing_values, column_idx, expected_type));
    else
    {
        ColumnPtr evaluated = templates[column_idx]->evaluateAll(block_missing_values, column_idx, expected_type, column->size());
        column->insertRangeFrom(*evaluated, 0, evaluated->size());
    }
    /// Do not use this template anymore
    templates[column_idx].reset();
    buf->rollbackToCheckpoint();
    token_iterator = start;

    /// It will deduce new template or fallback to slow SQL parser
    return parseExpression(*column, column_idx);
}

bool ValuesBlockInputFormat::tryReadValue(IColumn & column, size_t column_idx)
{
    bool rollback_on_exception = false;
    try
    {
        bool read = true;
        if (bool default_value = checkStringByFirstCharacterAndAssertTheRestCaseInsensitive("DEFAULT", *buf); default_value)
        {
            column.insertDefault();
            read = false;
        }
        else
        {
            const auto & type = types[column_idx];
            const auto & serialization = serializations[column_idx];
            if (format_settings.null_as_default && !isNullableOrLowCardinalityNullable(type))
                read = SerializationNullable::deserializeNullAsDefaultOrNestedTextQuoted(column, *buf, format_settings, serialization);
            else
                serialization->deserializeTextQuoted(column, *buf, format_settings);
        }

        rollback_on_exception = true;

        skipWhitespaceIfAny(*buf);
        assertDelimiterAfterValue(column_idx);
        return read;
    }
    catch (const Exception & e)
    {
        /// Do not consider decimal overflow as parse error to avoid attempts to parse it as expression with float literal
        bool decimal_overflow = e.code() == ErrorCodes::ARGUMENT_OUT_OF_BOUND;
        if (!isParseError(e.code()) || decimal_overflow)
            throw;
        if (rollback_on_exception)
            column.popBack(1);

        /// Switch to SQL parser and don't try to use streaming parser for complex expressions
        /// Note: Throwing exceptions for each expression may be very slow because of stacktraces
        buf->rollbackToCheckpoint();
        return parseExpression(column, column_idx);
    }
}

namespace
{
    void tryToReplaceNullFieldsInComplexTypesWithDefaultValues(Field & value, const IDataType & data_type)
    {
        checkStackSize();

        WhichDataType type(data_type);

        if (type.isTuple() && value.getType() == Field::Types::Tuple)
        {
            const DataTypeTuple & type_tuple = static_cast<const DataTypeTuple &>(data_type);

            Tuple & tuple_value = value.safeGet<Tuple>();

            size_t src_tuple_size = tuple_value.size();
            size_t dst_tuple_size = type_tuple.getElements().size();

            if (src_tuple_size != dst_tuple_size)
                throw Exception(ErrorCodes::TYPE_MISMATCH, "Bad size of tuple. Expected size: {}, actual size: {}.",
                    src_tuple_size, dst_tuple_size);

            for (size_t i = 0; i < src_tuple_size; ++i)
            {
                const auto & element_type = *(type_tuple.getElements()[i]);

                if (tuple_value[i].isNull() && !element_type.isNullable())
                    tuple_value[i] = element_type.getDefault();

                tryToReplaceNullFieldsInComplexTypesWithDefaultValues(tuple_value[i], element_type);
            }
        }
        else if (type.isArray() && value.getType() == Field::Types::Array)
        {
            const DataTypeArray & type_aray = static_cast<const DataTypeArray &>(data_type);
            const auto & element_type = *(type_aray.getNestedType());

            if (element_type.isNullable())
                return;

            Array & array_value = value.safeGet<Array>();
            size_t array_value_size = array_value.size();

            for (size_t i = 0; i < array_value_size; ++i)
            {
                if (array_value[i].isNull())
                    array_value[i] = element_type.getDefault();

                tryToReplaceNullFieldsInComplexTypesWithDefaultValues(array_value[i], element_type);
            }
        }
        else if (type.isMap() && value.getType() == Field::Types::Map)
        {
            const DataTypeMap & type_map = static_cast<const DataTypeMap &>(data_type);

            const auto & key_type = *type_map.getKeyType();
            const auto & value_type = *type_map.getValueType();

            auto & map = value.safeGet<Map>();
            size_t map_size = map.size();

            for (size_t i = 0; i < map_size; ++i)
            {
                auto & map_entry = map[i].safeGet<Tuple>();

                auto & entry_key = map_entry[0];
                auto & entry_value = map_entry[1];

                if (entry_key.isNull() && !key_type.isNullable())
                    entry_key = key_type.getDefault();

                tryToReplaceNullFieldsInComplexTypesWithDefaultValues(entry_key, key_type);

                if (entry_value.isNull() && !value_type.isNullable())
                    entry_value = value_type.getDefault();

                tryToReplaceNullFieldsInComplexTypesWithDefaultValues(entry_value, value_type);
            }
        }
    }
}

bool ValuesBlockInputFormat::parseExpression(IColumn & column, size_t column_idx)
{
    const Block & header = getPort().getHeader();
    const IDataType & type = *header.getByPosition(column_idx).type;
    const auto & settings = context->getSettingsRef();

    /// Advance the token iterator until the start of the column expression
    readUntilTheEndOfRowAndReTokenize(column_idx);

    bool parsed = false;
    ASTPtr ast;
    std::optional<IParser::Pos> ti_start;

    if (!(*token_iterator)->isError() && !(*token_iterator)->isEnd())
    {
        Expected expected;
        /// Keep a copy to the start of the column tokens to use if later if necessary
        ti_start = IParser::Pos(
            *token_iterator, static_cast<unsigned>(settings[Setting::max_parser_depth]), static_cast<unsigned>(settings[Setting::max_parser_backtracks]));

        parsed = parser.parse(*token_iterator, ast, expected);

        /// Consider delimiter after value (',' or ')') as part of expression
        if (column_idx + 1 != num_columns)
            parsed &= (*token_iterator)->type == TokenType::Comma;
        else
            parsed &= (*token_iterator)->type == TokenType::ClosingRoundBracket;
    }

    if (!parsed)
        throw Exception(
            ErrorCodes::SYNTAX_ERROR,
            "Cannot parse expression of type {} here: {}",
            type.getName(),
            std::string_view(buf->position(), std::min(SHOW_CHARS_ON_SYNTAX_ERROR, buf->buffer().end() - buf->position())));
    ++(*token_iterator);

    if (parser_type_for_column[column_idx] != ParserType::Streaming && dynamic_cast<const ASTLiteral *>(ast.get()))
    {
        /// It's possible that streaming parsing has failed on some row (e.g. because of '+' sign before integer),
        /// but it still can parse the following rows
        /// Check if we can use fast streaming parser instead if using templates
        bool rollback_on_exception = false;
        bool ok = false;
        try
        {
            const auto & serialization = serializations[column_idx];
            serialization->deserializeTextQuoted(column, *buf, format_settings);
            rollback_on_exception = true;
            skipWhitespaceIfAny(*buf);
            if (checkDelimiterAfterValue(column_idx))
                ok = true;
        }
        catch (const Exception & e)
        {
            bool decimal_overflow = e.code() == ErrorCodes::ARGUMENT_OUT_OF_BOUND;
            if (!isParseError(e.code()) || decimal_overflow)
                throw;
        }
        if (ok)
        {
            parser_type_for_column[column_idx] = ParserType::Streaming;
            return true;
        }
        if (rollback_on_exception)
            column.popBack(1);
    }

    parser_type_for_column[column_idx] = ParserType::SingleExpressionEvaluation;

    /// Try to deduce template of expression and use it to parse the following rows
    if (shouldDeduceNewTemplate(column_idx))
    {
        if (templates[column_idx])
            throw DB::Exception(ErrorCodes::LOGICAL_ERROR, "Template for column {} already exists and it was not evaluated yet",
                                std::to_string(column_idx));
        std::exception_ptr exception;
        try
        {
            bool found_in_cache = false;
            const auto & result_type = header.getByPosition(column_idx).type;
            const char * delimiter = (column_idx + 1 == num_columns) ? ")" : ",";
            auto structure = templates_cache.getFromCacheOrConstruct(
                result_type,
                !result_type->isNullable() && format_settings.null_as_default,
                *ti_start,
                *token_iterator,
                ast,
                context,
                &found_in_cache,
                delimiter);

            LOG_TEST(getLogger("ValuesBlockInputFormat"), "Will use an expression template to parse column {}: {}",
                     column_idx, structure->dumpTemplate());

            templates[column_idx].emplace(structure);
            if (found_in_cache)
                ++attempts_to_deduce_template_cached[column_idx];
            else
                ++attempts_to_deduce_template[column_idx];

            buf->rollbackToCheckpoint();
            if (templates[column_idx]->parseExpression(*buf, *ti_start, format_settings, settings))
            {
                ++rows_parsed_using_template[column_idx];
                parser_type_for_column[column_idx] = ParserType::BatchTemplate;
                return true;
            }
        }
        catch (...)
        {
            exception = std::current_exception();
        }
        if (!format_settings.values.interpret_expressions)
        {
            if (exception)
                std::rethrow_exception(exception);
            else
            {
                buf->rollbackToCheckpoint();
                size_t len = const_cast<char *>((*token_iterator)->begin) - buf->position();
                throw Exception(ErrorCodes::SYNTAX_ERROR, "Cannot deduce template of expression: {}", std::string(buf->position(), len));
            }
        }
        /// Continue parsing without template
        templates[column_idx].reset();
    }

    if (!format_settings.values.interpret_expressions)
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Interpreting expressions is disabled");

    /// Try to evaluate single expression if other parsers don't work
    buf->position() = const_cast<char *>((*token_iterator)->begin);

    std::pair<Field, DataTypePtr> value_raw = evaluateConstantExpression(ast, context);

    Field & expression_value = value_raw.first;

    if (format_settings.null_as_default)
        tryToReplaceNullFieldsInComplexTypesWithDefaultValues(expression_value, type);

    Field value = convertFieldToType(expression_value, type, value_raw.second.get(), format_settings);

    /// Check that we are indeed allowed to insert a NULL.
    if (value.isNull() && !type.isNullable() && !type.isLowCardinalityNullable())
    {
        if (format_settings.null_as_default)
        {
            type.insertDefaultInto(column);
            return false;
        }
        buf->rollbackToCheckpoint();
        throw Exception(ErrorCodes::TYPE_MISMATCH, "Cannot insert NULL value into a column of type '{}' at: {}",
                        type.getName(), String(buf->position(),
                        std::min(SHOW_CHARS_ON_SYNTAX_ERROR, buf->buffer().end() - buf->position())));
    }

    column.insert(value);
    return true;
}

void ValuesBlockInputFormat::assertDelimiterAfterValue(size_t column_idx)
{
    if (unlikely(!checkDelimiterAfterValue(column_idx)))
        throwAtAssertionFailed((column_idx + 1 == num_columns) ? ")" : ",", *buf);
}

bool ValuesBlockInputFormat::checkDelimiterAfterValue(size_t column_idx)
{
    skipWhitespaceIfAny(*buf);

    if (likely(column_idx + 1 != num_columns))
    {
        return checkChar(',', *buf);
    }

    /// Optional trailing comma.
    if (checkChar(',', *buf))
        skipWhitespaceIfAny(*buf);
    return checkChar(')', *buf);
}

bool ValuesBlockInputFormat::shouldDeduceNewTemplate(size_t column_idx)
{
    if (!format_settings.values.deduce_templates_of_expressions)
        return false;

    /// TODO better heuristic

    /// Using template from cache is approx 2x faster, than evaluating single expression
    /// Construction of new template is approx 1.5x slower, than evaluating single expression
    double attempts_weighted = 1.5 * attempts_to_deduce_template[column_idx] +  0.5 * attempts_to_deduce_template_cached[column_idx];

    constexpr size_t max_attempts = 100;
    if (attempts_weighted < max_attempts)
        return true;

    if (rows_parsed_using_template[column_idx] / attempts_weighted > 1)
    {
        /// Try again
        attempts_to_deduce_template[column_idx] = 0;
        attempts_to_deduce_template_cached[column_idx] = 0;
        rows_parsed_using_template[column_idx] = 0;
        return true;
    }
    return false;
}

void ValuesBlockInputFormat::readPrefix()
{
    /// In this format, BOM at beginning of stream cannot be confused with value, so it is safe to skip it.
    skipBOMIfExists(*buf);
}

void ValuesBlockInputFormat::readSuffix()
{
    if (!buf->eof() && *buf->position() == ';')
    {
        ++buf->position();
        skipWhitespaceIfAny(*buf);
        if (buf->hasUnreadData())
            throw Exception(ErrorCodes::CANNOT_READ_ALL_DATA, "Cannot read data after semicolon");
        return;
    }

    if (buf->hasUnreadData() || !buf->eof())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unread data in PeekableReadBuffer will be lost. Most likely it's a bug.");
}

void ValuesBlockInputFormat::resetParser()
{
    if (got_exception)
    {
        /// In case of exception always reset the templates and parser type,
        /// because they may be in the invalid state.
        for (size_t i = 0; i < num_columns; ++i)
        {
            templates[i].reset();
            parser_type_for_column[i] = ParserType::Streaming;
        }
    }

    IInputFormat::resetParser();
    // I'm not resetting parser modes here.
    // There is a good chance that all messages have the same format.
    total_rows = 0;
}

void ValuesBlockInputFormat::setReadBuffer(ReadBuffer & in_)
{
    buf = std::make_unique<PeekableReadBuffer>(in_);
    IInputFormat::setReadBuffer(*buf);
}

void ValuesBlockInputFormat::resetReadBuffer()
{
    buf.reset();
    IInputFormat::resetReadBuffer();
}

void ValuesBlockInputFormat::setContext(const ContextPtr & context_)
{
    context = Context::createCopy(context_);
}

void ValuesBlockInputFormat::setQueryParameters(const NameToNameMap & parameters)
{
    if (parameters == context->getQueryParameters())
        return;

    auto context_copy = Context::createCopy(context);
    context_copy->setQueryParameters(parameters);
    context = std::move(context_copy);
}

ValuesSchemaReader::ValuesSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_)
    : IRowSchemaReader(buf, format_settings_), buf(in_)
{
}

std::optional<DataTypes> ValuesSchemaReader::readRowAndGetDataTypes()
{
    if (first_row)
    {
        skipBOMIfExists(buf);
        first_row = false;
    }

    skipWhitespaceIfAny(buf);
    if (buf.eof() || end_of_data)
        return {};

    assertChar('(', buf);
    skipWhitespaceIfAny(buf);
    DataTypes data_types;
    String value;
    while (!buf.eof() && *buf.position() != ')')
    {
        if (!data_types.empty())
        {
            assertChar(',', buf);
            skipWhitespaceIfAny(buf);
        }

        readQuotedField(value, buf);
        auto type = tryInferDataTypeByEscapingRule(value, format_settings, FormatSettings::EscapingRule::Quoted);
        data_types.push_back(std::move(type));
        skipWhitespaceIfAny(buf);
    }

    assertChar(')', buf);

    skipWhitespaceIfAny(buf);
    if (!buf.eof() && *buf.position() == ',')
        ++buf.position();

    if (!buf.eof() && *buf.position() == ';')
    {
        ++buf.position();
        end_of_data = true;
    }

    return data_types;
}

void ValuesSchemaReader::transformTypesIfNeeded(DB::DataTypePtr & type, DB::DataTypePtr & new_type)
{
    transformInferredTypesIfNeeded(type, new_type, format_settings);
}

void registerInputFormatValues(FormatFactory & factory)
{
    factory.registerInputFormat("Values", [](
        ReadBuffer & buf,
        const Block & header,
        const RowInputFormatParams & params,
        const FormatSettings & settings)
    {
        return std::make_shared<ValuesBlockInputFormat>(buf, std::make_unique<const Block>(header), params, settings);
    });
}

void registerValuesSchemaReader(FormatFactory & factory)
{
    factory.registerSchemaReader("Values", [](ReadBuffer & buf, const FormatSettings & settings)
    {
        return std::make_shared<ValuesSchemaReader>(buf, settings);
    });
    factory.registerAdditionalInfoForSchemaCacheGetter("Values", [](const FormatSettings & settings)
    {
        return getAdditionalFormatInfoByEscapingRule(settings, FormatSettings::EscapingRule::Quoted);
    });
}

}
