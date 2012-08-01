#include <queue>
#include <iomanip>

#include <DB/DataStreams/MergingSortedBlockInputStream.h>


namespace DB
{

Block MergingSortedBlockInputStream::readImpl()
{
	if (!inputs.size())
		return Block();
	
	if (inputs.size() == 1)
		return inputs[0]->read();

	/// Читаем первые блоки, инициализируем очередь.
	if (first)
	{
		first = false;
		size_t i = 0;
		for (Blocks::iterator it = source_blocks.begin(); it != source_blocks.end(); ++it, ++i)
		{
			if (*it)
				continue;

			*it = inputs[i]->read();

			if (!*it)
				continue;

			if (!num_columns)
				num_columns = source_blocks[0].columns();

			cursors[i] = SortCursorImpl(*it, description);
			queue.push(SortCursor(&cursors[i]));
		}
	}

	/// Инициализируем результат.
	size_t merged_rows = 0;
	Block merged_block;
	ColumnPlainPtrs merged_columns;

	/// Клонируем структуру первого непустого блока источников.
	{
		Blocks::const_iterator it = source_blocks.begin();
		for (; it != source_blocks.end(); ++it)
		{
			if (*it)
			{
				merged_block = it->cloneEmpty();
				break;
			}
		}

		/// Если все входные блоки пустые.
		if (it == source_blocks.end())
			return Block();
	}
		
	for (size_t i = 0; i < num_columns; ++i)
		merged_columns.push_back(&*merged_block.getByPosition(i).column);

	/// Вынимаем строки в нужном порядке и кладём в merged_block, пока строк не больше max_block_size
	while (!queue.empty())
	{
		SortCursor current = queue.top();
		queue.pop();

		for (size_t i = 0; i < num_columns; ++i)
			merged_columns[i]->insert((*current->all_columns[i])[current->pos]);

		if (!current->isLast())
		{
			current->next();
			queue.push(current);
		}
		else
		{
			/// Достаём из соответствующего источника следующий блок, если есть.

			size_t i = 0;
			size_t size = cursors.size();
			for (; i < size; ++i)
			{
				if (&cursors[i] == current.impl)
				{
					source_blocks[i] = inputs[i]->read();
					if (source_blocks[i])
					{
						cursors[i].reset(source_blocks[i]);
						queue.push(SortCursor(&cursors[i]));
					}

					break;
				}
			}

			if (i == size)
				throw Exception("Logical error in MergingSortedBlockInputStream", ErrorCodes::LOGICAL_ERROR);
		}

		++merged_rows;
		if (merged_rows == max_block_size)
			return merged_block;
	}

	const BlockStreamProfileInfo & profile_info = getInfo();
	double seconds = profile_info.work_stopwatch.elapsedSeconds();
	LOG_DEBUG(log, std::fixed << std::setprecision(2)
		<< "Merge sorted " << profile_info.blocks << " blocks, " << profile_info.rows << " rows"
		<< " in " << seconds << " sec., "
		<< profile_info.rows / seconds << " rows/sec., "
		<< profile_info.bytes / 1000000.0 / seconds << " MiB/sec.");

	inputs.clear();
	return merged_block;
}

}
