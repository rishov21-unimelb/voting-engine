/*
 * main.c -- application entry point.
 *
 * Deliberately empty of logic: everything the program does lives in the
 * library, which keeps the engine linkable into tests and into other
 * front ends without dragging a main() along with it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "voting.h"

int main(int argc, char **argv)
{
    return vote_cli_main(argc, argv);
}
