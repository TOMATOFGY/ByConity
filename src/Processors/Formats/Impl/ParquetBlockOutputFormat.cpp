#include "ParquetBlockOutputFormat.h"

#if USE_PARQUET

// TODO: clean includes
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Common/assert_cast.h>
#include <Core/callOnTypeIndex.h>
#include <DataStreams/SquashingBlockOutputStream.h>
#include <Formats/FormatFactory.h>
#include <IO/WriteHelpers.h>
#include <arrow/api.h>
#include <arrow/util/memory.h>
#include <parquet/arrow/writer.h>
#include "ArrowBufferedStreams.h"
#include "CHColumnToArrowColumn.h"


namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_EXCEPTION;
}

ParquetBlockOutputFormat::ParquetBlockOutputFormat(WriteBuffer & out_, const Block & header_, const FormatSettings & format_settings_)
    : IOutputFormat(header_, out_), format_settings{format_settings_}
{
}

void ParquetBlockOutputFormat::consume(Chunk chunk)
{
    const size_t columns_num = chunk.getNumColumns();
    std::shared_ptr<arrow::Table> arrow_table;

    if (!ch_column_to_arrow_column)
    {
        const Block & header = getPort(PortKind::Main).getHeader();
        ch_column_to_arrow_column = std::make_unique<CHColumnToArrowColumn>(header, "Parquet");
    }

    ch_column_to_arrow_column->chChunkToArrowTable(arrow_table, chunk, columns_num);

    if (!file_writer)
    {
        auto sink = std::make_shared<ArrowBufferedOutputStream>(out);

        parquet::WriterProperties::Builder builder;
#if USE_SNAPPY
        builder.compression(parquet::Compression::SNAPPY);
#endif
        auto props = builder.build();
        auto status = parquet::arrow::FileWriter::Open(
            *arrow_table->schema(),
            arrow::default_memory_pool(),
            sink,
            props /*parquet::default_writer_properties(),*/
            ).Value(&file_writer);
        if (!status.ok())
            throw Exception{"Error while opening a table: " + status.ToString(), ErrorCodes::UNKNOWN_EXCEPTION};
        
    }

    // TODO: calculate row_group_size depending on a number of rows and table size
    auto status = file_writer->WriteTable(*arrow_table, format_settings.parquet.row_group_size);

    if (!status.ok())
        throw Exception{"Error while writing a table: " + status.ToString(), ErrorCodes::UNKNOWN_EXCEPTION};
}

void ParquetBlockOutputFormat::finalize()
{
    if (!file_writer)
    {
        const Block & header = getPort(PortKind::Main).getHeader();

        consume(Chunk(header.getColumns(), 0));
    }

    auto status = file_writer->Close();
    if (!status.ok())
        throw Exception{"Error while closing a table: " + status.ToString(), ErrorCodes::UNKNOWN_EXCEPTION};
}

void registerOutputFormatProcessorParquet(FormatFactory & factory)
{
    factory.registerOutputFormatProcessor(
        "Parquet",
        [](WriteBuffer & buf,
           const Block & sample,
           const RowOutputFormatParams &,
           const FormatSettings & format_settings)
        {
            auto impl = std::make_shared<ParquetBlockOutputFormat>(buf, sample, format_settings);
            /// TODO
            // auto res = std::make_shared<SquashingBlockOutputStream>(impl, impl->getHeader(), format_settings.parquet.row_group_size, 0);
            // res->disableFlush();
            return impl;
        });
}

}

#else

namespace DB
{
class FormatFactory;
void registerOutputFormatProcessorParquet(FormatFactory &)
{
}
}

#endif
