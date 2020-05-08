#include "damage.h"
#include "tst.h"

#include <pixman.h>

static int test_damage_check_row_aligned(void)
{
	uint32_t width = 32;
	uint8_t row[width];
	uint8_t r = 0;

	memset(row, 0, width);
	damage_check_row(&r, row, width);

	ASSERT_FALSE(r);

	row[0] = 1;
	r = 0;
	damage_check_row(&r, row, width);

	ASSERT_TRUE(r);

	return 0;
}

static int test_damage_check_row_unaligned(void)
{
	uint32_t width = 33;
	uint8_t row[width];
	uint8_t r[2] = { 0 };

	memset(row, 0, width);
	damage_check_row(r, row, width);

	ASSERT_FALSE(r[0]);
	ASSERT_FALSE(r[1]);

	row[32] = 1;
	r[0] = 0;
	r[1] = 0;
	damage_check_row(r, row, width);

	ASSERT_FALSE(r[0]);
	ASSERT_TRUE(r[1]);

	return 0;
}

static int test_damage_check_tile_row_aligned(void)
{
	uint32_t width = 32;
	uint32_t height = 32;
	uint8_t tile[width * height];
	struct pixman_region16 damage;
	uint8_t row = 0;
	pixman_region_init(&damage);

	memset(tile, 0, sizeof(tile));
	damage_check_tile_row(&damage, &row, tile, 0, width, height);
	ASSERT_FALSE(pixman_region_not_empty(&damage));

	row = 0;
	pixman_region_clear(&damage);
	tile[0] = 1;
	damage_check_tile_row(&damage, &row, tile, 0, width, height);
	ASSERT_TRUE(pixman_region_not_empty(&damage));

	pixman_region_fini(&damage);
	return 0;
}

static int test_damage_check_tile_row_unaligned(void)
{
	uint32_t width = 33;
	uint32_t height = 32;
	uint8_t tile[width * height];
	struct pixman_region16 damage;
	uint8_t row[2] = { 0 };
	pixman_region_init(&damage);

	memset(tile, 0, sizeof(tile));
	damage_check_tile_row(&damage, row, tile, 0, width, height);
	ASSERT_FALSE(pixman_region_not_empty(&damage));

	row[0] = 0;
	row[1] = 0;
	pixman_region_clear(&damage);
	tile[32] = 1;
	damage_check_tile_row(&damage, row, tile, 0, width, height);
	ASSERT_TRUE(pixman_region_not_empty(&damage));

	pixman_region_fini(&damage);
	return 0;
}

int main()
{
	int r = 0;
	RUN_TEST(test_damage_check_row_aligned);
	RUN_TEST(test_damage_check_row_unaligned);
	RUN_TEST(test_damage_check_tile_row_aligned);
	RUN_TEST(test_damage_check_tile_row_unaligned);
	return r;
}
