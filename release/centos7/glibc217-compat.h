/* Force-included into every OpenBF2 source file of the CentOS 7 build (release/centos7/build-in-builder.sh).
 *
 * In glibc 2.17, htons, htonl, ntohs and ntohl are macros that expand to a statement expression when optimising, so
 * the engine's "::htons(port)" does not parse (UDPSocket.cpp, NetServer.cpp). Without the macros the same names are
 * the real functions, which is what a current glibc gives the regular release as well (inline functions there).
 */
#include <netinet/in.h>
#undef htons
#undef htonl
#undef ntohs
#undef ntohl
