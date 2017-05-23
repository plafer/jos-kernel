#include <inc/lib.h>

#define MTXSIZ 3

envid_t top_level_env;

uint32_t A[MTXSIZ][MTXSIZ] = { {1, 0, 0},
			       {0, 1, 0},
			       {0, 0, 1},
};

uint32_t IN[MTXSIZ][MTXSIZ] = { {1, 2, 3},
				{4, 5, 6},
				{7, 8, 9},
};

// Written by south
uint32_t RESULT[MTXSIZ][MTXSIZ];

envid_t north[MTXSIZ];
envid_t west[MTXSIZ];
envid_t center[MTXSIZ][MTXSIZ];

envid_t east[MTXSIZ];
envid_t south[MTXSIZ];

// In shared page
#define BASE    ((envid_t *) 0xee000000)
#define NORTH   ((envid_t *) BASE)
#define WEST    ((envid_t *) ((char *)NORTH + sizeof(north)))
#define CENTER  ((envid_t *) ((char *)WEST + sizeof(west)))
#define EAST    ((envid_t *) ((char *)CENTER + sizeof(center)))
#define SOUTH   ((envid_t *) ((char *)EAST + sizeof(east)))

static void do_north_stuff(int col)
{
	int i;
	envid_t from_env;

	ipc_recv(&from_env, BASE, NULL);

	if (from_env != top_level_env)
	{
		panic("North received message from environment "
		      "other than top level env: %d", from_env);
	}

	cprintf("North %d received mapping.\n", col);

	// Wait for 'go' from top level env
	ipc_recv(&from_env, NULL, 0);
	if (from_env != top_level_env)
	{
		panic("North received message from environment "
		      "other than top level env: %d. Was expecting "
		      "'Go'", from_env);
	}

	cprintf("North %d received 'go' from top level env.\n", col);

	for (i = 0; i < MTXSIZ; i++)
		// CENTER[0][col]
		ipc_send(*(CENTER + col), 0, NULL, 0);
}


static void fork_north(void)
{
	int i;

	for (i = 0; i < MTXSIZ; i++)
	{
		envid_t child;

		if ((child = fork()) == 0)
		{
			do_north_stuff(i);
			exit();
		}
		else
		{
			north[i] = child;
		}
	}
}

static void do_east_stuff(void)
{
	while(1)
		// Sink
		ipc_recv(NULL, NULL, NULL);
}


static void fork_east(void)
{
	int i;

	for (i = 0; i < MTXSIZ; i++)
	{
		envid_t child;

		if ((child = fork()) == 0)
		{
			do_east_stuff();
			exit();
		}
		else
		{
			east[i] = child;
		}
	}
}

static void do_center_stuff(int row, int col)
{
	envid_t north_env;
	envid_t east_env;
	envid_t south_env;
	envid_t west_env;

	int32_t vectors[MTXSIZ];
	uint32_t partial_sums[MTXSIZ];

	envid_t from_env;
	int32_t input;

	int vec_i = 0;
	int ps_i = 0;
	int consume_i = 0;

	ipc_recv(&from_env, BASE, NULL);
	if (from_env != top_level_env)
		panic("Center row=%d, col=%d expected a page mapping message "
		      "from top level environment", row, col);

	cprintf("Center row=%d, col=%d received mapping\n", row, col);

	// Setup north env
	if (row == 0)
		north_env = NORTH[col];
	else
		// CENTER[row - 1][col]
		north_env = *(CENTER + ((row - 1) * MTXSIZ) + col);

	// Setup east env
	if (col == MTXSIZ - 1)
		east_env = EAST[row];
	else
		// CENTER[row][col + 1]
		east_env = *(CENTER + (row * MTXSIZ) + (col + 1));

	// Setup south env
	if (row == MTXSIZ - 1)
		south_env = SOUTH[col];
	else
		// CENTER[row + 1][col]
		south_env = *(CENTER + ((row + 1) * MTXSIZ) + col);

	// Setup west env
	if (col == 0)
		west_env = WEST[row];
	else
		// CENTER[row][col - 1]
		west_env = *(CENTER + (row * MTXSIZ) + (col - 1));

	while (1)
	{
		input = ipc_recv(&from_env, NULL, NULL);

		// The ps_i < MTXSIZ check will throw away any excess input that
		// would come, for example, from the constant North stream of 0s
		if (from_env == north_env && ps_i < MTXSIZ)
		{
			partial_sums[ps_i] = input;
			ps_i++;
		}
		else if (from_env == west_env && vec_i < MTXSIZ)
		{
			vectors[vec_i] = input;
			ipc_send(east_env, vectors[vec_i], NULL, 0);
			vec_i++;
		}

		if (consume_i < vec_i && consume_i < ps_i)
		{
			uint32_t new_ps = partial_sums[consume_i] +
				vectors[consume_i] * A[row][col];

			ipc_send(south_env, new_ps, NULL, 0);

			consume_i++;
			if (consume_i == MTXSIZ)
			{
				// Input matrix done, reset
				consume_i = 0;
				vec_i = 0;
				ps_i = 0;
			}
		}
	}
}

static void fork_center(void)
{
	int i;
	int j;

	for (i = 0; i < MTXSIZ; i++)
	{
		for (j = 0; j < MTXSIZ; j++)
		{
			envid_t child;

			if ((child = fork()) == 0)
			{
				do_center_stuff(i, j);
				exit();
			}
			else
			{
				center[i][j] = child;
			}
		}
	}

}

static void do_west_stuff(int col)
{
	envid_t from_env;
	int row;

	// Wait for 'go' from top level env
	ipc_recv(&from_env, NULL, 0);
	if (from_env != top_level_env)
	{
		panic("West received message from environment "
		      "other than top level env: %d. Was expecting "
		      "'Go'", from_env);
	}

	cprintf("West received 'go' from top level env.\n");

	for (row = 0; row < MTXSIZ; row++)
		// send to center at your column index's row.
		ipc_send(center[col][0], IN[row][col], NULL, 0);

}

static void fork_west(void)
{
	int i;

	for (i = 0; i < MTXSIZ; i++)
	{
		envid_t child;

		if ((child = fork()) == 0)
		{
			do_west_stuff(i);
			exit();
		}
		else
		{
			west[i] = child;
		}
	}
}

static void do_south_stuff(int col)
{
	while (1)
	{
		int row;

		for (row = 0; row < MTXSIZ; row++)
		{
			int32_t val;
			val = ipc_recv(NULL, NULL, NULL);

			ipc_send(top_level_env, val, NULL, 0);
		}
	}
}

static void fork_south(void)
{
	int i;

	for (i = 0; i < MTXSIZ; i++)
	{
		envid_t child;

		if ((child = fork()) == 0)
		{
			do_south_stuff(i);
			exit();
		}
		else
		{
			south[i] = child;
		}
	}
}

static void setup_shared_page(void)
{
	int i;
	int j;

	if (sys_page_alloc(0, BASE, PTE_U | PTE_W) < 0)
		panic("Failed to allocate shared page");

	// Set up shared page
	memcpy(NORTH, north, sizeof(north));
	memcpy(WEST, west, sizeof(west));
	memcpy(CENTER, center, sizeof(center));
	memcpy(EAST, east, sizeof(east));
	memcpy(SOUTH, south, sizeof(south));

	// Set up shared page in north
	for (i = 0; i < MTXSIZ; i++)
		ipc_send(north[i], 0, BASE, PTE_U);

	// East, South and West expect nothing

	// Set up shared page in center
	for (i = 0; i < MTXSIZ; i++)
	{
		for (j = 0; j < MTXSIZ; j++)
		{
			ipc_send(center[i][j], 0, BASE, PTE_U);
		}
	}
}

void umain(int argc, char **argv)
{
	int i;
	int j;
	int k;

	top_level_env = thisenv->env_id;

	fork_north();
	fork_east();
	fork_center();

	// These would be user processes normally
	// We'll do it hacky: since the center array is already initialized when
	// we fork west and south, we'll have them access the center array
	// directly.
	fork_west();
	fork_south();

	setup_shared_page();

	// Start the machine
	for (i = 0; i < MTXSIZ; i++)
	{
		ipc_send(north[i], 0, NULL, 0);
		ipc_send(west[i], 0, NULL, 0);
	}


	// Receive messages from three south ridges
	for (i = 0, j = 0, k = 0; i < MTXSIZ || j < MTXSIZ || k < MTXSIZ; )
	{
		envid_t from_env;
		int32_t val;

		val = ipc_recv(&from_env, NULL, NULL);
		if (from_env == south[0])
			RESULT[i++][0] = val;
		else if (from_env == south[1])
			RESULT[j++][1] = val;
		else if (from_env == south[2])
			RESULT[k++][2] = val;
		else
			panic("Unexpected message from non-south env: %d",
			      from_env);
	}


	// print result
	cprintf("RESULT:\n");
	for (i = 0; i < MTXSIZ; i++)
	{
		for (j = 0; j < MTXSIZ; j++)
			cprintf("%d", RESULT[i][j]);
		cprintf("\n");
	}
}
