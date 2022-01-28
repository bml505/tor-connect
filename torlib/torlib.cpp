#include "torlib.h"
#include "Curve25519.h"
#include "RelayCell.h"


bool TorLib::Init()
{
  boost::log::trivial::severity_level log_level = boost::log::trivial::info;
	// with this filter
	auto filt = boost::log::filter(boost::log::trivial::severity >= log_level);
	boost::log::core::get()->set_filter(filt);

	BOOST_LOG_TRIVIAL(debug) << "TorLib::Init";

	DA.push_back(make_tuple("gabelmoo", "131.188.40.189", 443, 80));
	DA.push_back(make_tuple("moria1", "128.31.0.39", 9101, 9131));
	DA.push_back(make_tuple("tor26", "86.59.21.38", 443, 80));
	DA.push_back(make_tuple("dizum", "194.109.206.212", 443, 80));
	DA.push_back(make_tuple("Tonga", "82.94.251.203", 443, 80));	
	DA.push_back(make_tuple("dannenberg", "193.23.244.244", 443, 80));
	DA.push_back(make_tuple("maatuska", "171.25.193.9", 80, 443));
	DA.push_back(make_tuple("Faravahar", "154.35.175.225", 443, 80));
	DA.push_back(make_tuple("longclaw", "199.254.238.52", 443, 80));

	circuit_id |= 0x80000000;

	return true;
}

int TorLib::Connect(const string ip, const int port, const int timeout)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::Connect";
	BOOST_LOG_TRIVIAL(info) << "Connect to "<< ip <<":"<< port;
	stream_host = ip;
	stream_port = port;
	work = make_shared<net::io_service::work>(io_service);

	//work(io_service);

	error_last_operation = false;
	BOOST_LOG_TRIVIAL(info) << "Retrieving the file consensus...";
	// Get Consensus
	if(!GetConsensus()) return 1;
	
	// Node 1
	BOOST_LOG_TRIVIAL(info) << "Connect to Node 1 ...";
	// Connect To Node
	if (!ConnectToNode(1)) return 2;
	if(!SendNodeInfo(boost::bind(&TorLib::LogErr, this, pl::error))) return 3;
	// Get keys	
	if (!GetKeysNode(1)) return 4;
	
	if (!CreateNtor(1, boost::bind(&TorLib::LogErr, this, pl::error))) return 5;

	BOOST_LOG_TRIVIAL(debug) << "Connect To Node 1 complite";

	// Node 2
	BOOST_LOG_TRIVIAL(info) << "Connect to Node 2 ...";
	// Connect To Node
	if (!ConnectToNode(2, port)) return 6;	
	// Get keys	
	if (!GetKeysNode(2)) return 7;

	if (!CreateExtendNtor(2, boost::bind(&TorLib::LogErr, this, pl::error))) return 8;

	BOOST_LOG_TRIVIAL(debug) << "Connect To Node 2 complite";

	BOOST_LOG_TRIVIAL(info) << "Create stream ...";
	n_stream = 1;
	if (!CreateStream(3, n_stream, ip, port, timeout, boost::bind(&TorLib::LogErr, this, pl::error))) return 9;
	
	BOOST_LOG_TRIVIAL(info) << "Connect completed.";	

	return 0;
}
bool TorLib::Receive(string& buff, const int timeout)
{
	buff = data_result;
	return true;
}
bool TorLib::Send(const string& req)
{
	//BOOST_LOG_TRIVIAL(debug) << "TorLib::Send : " << req;
	//string req = (boost::format("GET %1% HTTP/1.0\r\nHost: %2%\r\n\r\n") % path % stream_host).str();	
	bool ret_val = SendData(req, boost::bind(&TorLib::LogErr, this, pl::error));
	if (ret_val) BOOST_LOG_TRIVIAL(info) << "Request sent, data received.";
	return ret_val;
}
bool TorLib::SendData(string reqest, ConnectFunction connectFunc)
{
	operation_completed = false;
	BOOST_LOG_TRIVIAL(debug) << "TorLib::SendData reqest = " << reqest;
	RelayCell circuit_node(circuit_id, n_stream, cell_command::relay, cell_command::relay_data);
	circuit_node.Append(reqest);
	onion_routers[3]->Encrypt(circuit_node);
	onion_routers[2]->Encrypt(circuit_node, false);	
	net_connect->WriteCell(circuit_node,
		boost::bind(&TorLib::ReadStreamData, this, 3, connectFunc, pl::error));
	while (!operation_completed) io_service.poll_one();
	return !error_last_operation;
}
void TorLib::ReadStreamData(int n_node, ConnectFunction connectFunc, const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ReadStreamData";
	TEST_ERR(err, connectFunc);
	shared_ptr<Cell> node(new Cell());
	net_connect->ReadCell(node, boost::bind(&TorLib::ReadStreamComplete,
		this, n_node, connectFunc, node, pl::error));
}
void TorLib::ReadStreamComplete(int n_node, ConnectFunction connectFunc,
	shared_ptr<Cell> node, const sys::error_code& err)
{	
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ReadStreamCompleteP1";
	TEST_ERR(err, connectFunc);
	shared_ptr<RelayCell> relay_node(new RelayCell(*node));
	onion_routers[n_node]->Decrypt(*relay_node, false);
	onion_routers[n_node - 1]->Decrypt(*relay_node, false);
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ReadStreamComplete Command=" << static_cast<unsigned int>(relay_node->GetCommand());
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ReadStreamComplete Command Relay=" << static_cast<unsigned int>(relay_node->GetRelayType());

	BOOST_LOG_TRIVIAL(debug) << "-------------RelayCell-------------";
	Util::HexDump(relay_node->GetBuffer(), relay_node->GetBufferSize());
	BOOST_LOG_TRIVIAL(debug) << "-----------------------------------";

	data_result.clear();
	data_result.append(reinterpret_cast<char const*>(relay_node->GetPayload()), relay_node->GetPayloadSize());

	net::post(net::detail::bind_handler(connectFunc, err));
}
bool TorLib::CreateStream(int n_node, u16 id_stream, string host, int port, int timeout, ConnectFunction connectFunc)
{
	operation_completed = false;
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateStream to " << host << ":" << port;
	string host_port;
	host_port.append(host);
	host_port.append(":");
	host_port.append(std::to_string(port));

	RelayCell circuit_node(circuit_id, id_stream, cell_command::relay, cell_command::relay_begin);
	circuit_node.Append(host_port);
	onion_routers[n_node] = make_shared<OnionRouter>();
	onion_routers[n_node]->SetKeyMaterial(onion_routers[n_node - 1]->key_material.data());
	onion_routers[n_node]->Encrypt(circuit_node);
	onion_routers[n_node-1]->Encrypt(circuit_node, false);

	net_connect->WriteCell(circuit_node, 
		boost::bind(&TorLib::ReadStreamNode, this, n_node, connectFunc, pl::error));

	while (!operation_completed) io_service.poll_one();
	return !error_last_operation;
}
void TorLib::ReadStreamNode(int n_node, ConnectFunction connectFunc, const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ReadStreamNode";
	TEST_ERR(err, connectFunc);
	shared_ptr<Cell> node(new Cell());
	net_connect->ReadCell(node, boost::bind(&TorLib::CreateStreamComplete,
		this, n_node, connectFunc, node, pl::error));
}

void TorLib::CreateStreamComplete(int n_node, ConnectFunction connectFunc,
	shared_ptr<Cell> node, const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateStreamComplete";
	TEST_ERR(err, connectFunc);
	shared_ptr<RelayCell> relay_node(new RelayCell(*node));
	onion_routers[n_node]->Decrypt(*relay_node, false);
	onion_routers[n_node-1]->Decrypt(*relay_node);
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateStreamComplete Command=" << static_cast<unsigned int>(relay_node->GetCommand());
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateStreamComplete Command Relay=" << static_cast<unsigned int>(relay_node->GetRelayType());
	
	BOOST_LOG_TRIVIAL(debug) << "-------------RelayCell-------------";
	Util::HexDump(relay_node->GetBuffer(), relay_node->GetBufferSize());
	BOOST_LOG_TRIVIAL(debug) << "-----------------------------------";

	net::post(net::detail::bind_handler(connectFunc, err));	
}

bool TorLib::ConnectToNode(int n_node, int search_port)
{
	if (n_node == 1)
	{
		BOOST_LOG_TRIVIAL(debug) << "TorLib::ConnectToNode 1";

		ssl::context ctx(ssl::context::sslv23);
		//ctx.load_verify_file("torlib/cacert.pem");

		operation_completed = false;
		net_connect = make_unique<NetConnect>(io_service, ctx);
		onion_routers[n_node] = parser.GetOnionRouter(data_consensus, true, 443, 0, "", {"Fast","Running", "Valid"});

		if (onion_routers[n_node]->nickname.length() == 0)
		{
			BOOST_LOG_TRIVIAL(debug) << "Onion Router not found";
			return false;
		}
		BOOST_LOG_TRIVIAL(debug) << "Connecting to node " << n_node << ": '"
			<< onion_routers[n_node]->nickname << "' (" << onion_routers[n_node]->ip << ":" << onion_routers[n_node]->or_port << ")";
		net_connect->Connect(onion_routers[n_node]->ip, onion_routers[n_node]->or_port,
			boost::bind(&TorLib::LogErr, this, pl::error));

		while (!operation_completed) io_service.poll_one();
		return !error_last_operation;
	}
	if (n_node == 2)
	{		
		BOOST_LOG_TRIVIAL(debug) << "TorLib::ConnectToNode 2";
		onion_routers[n_node] = parser.GetOnionRouter(data_consensus, true, 0, 0, "", { "Exit", "Fast","Running", "Valid" }, search_port);
		if (onion_routers[n_node]->nickname.length() == 0)
		{
			BOOST_LOG_TRIVIAL(debug) << "Onion Router not found";
			return false;
		}
		return true;
	}
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ConnectToNode Incorrect call parameters";
	return false;
}

bool TorLib::GetKeysNode(int n_node)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::GetKeysNode";
	operation_completed = false;
	if (data_consensus.size() == 0)
	{
		BOOST_LOG_TRIVIAL(error) << "TorLib::GetKeysNode data_consensus empty";
		return false;
	}
	vector<string> data_node = parser.SearchOnionRouter(data_consensus, true, 0, 80, "", {});

	if (data_node.size() == 0)
	{
		BOOST_LOG_TRIVIAL(debug) << "Onion Router not found";
		return false;
	}
	string ip = data_node[static_cast<int>(entry_r_type::entry_r_ip)];
	int port = std::stoi(data_node[static_cast<int>(entry_r_type::entry_r_dir_port)]);
	BOOST_LOG_TRIVIAL(debug) << "Get Keys from Node " << n_node << ": '"
		<< data_node[static_cast<int>(entry_r_type::entry_r_nickname)] << "' ("	<< ip << ":"<< port << ")";
	string target = "/tor/server/fp/" + onion_routers[n_node]->GetBase16EncodedIdentity(onion_routers[n_node]->identity);
	data_result = GetDataFromUrl(ip, port, target);
	if (!data_result.empty())
	{
		BOOST_LOG_TRIVIAL(debug) << "TorLib::SetOnionRouterKeys";
		vector<string> data_node = parser.ParsString(data_result, "\n");
		if (data_node.size() == 0) return false;
		parser.SetOnionRouterKeys(onion_routers[n_node], data_node);
	}
	else
	{
		BOOST_LOG_TRIVIAL(debug) << "TorLib::ConnectToNodeComplete [data_result.empty()]";
		return false;
	}
	return true;
}

bool TorLib::GetConsensus()
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::GetConsensus";
	tuple<string, string, int, int> sv_one;
	int count_try = 0;
	data_consensus.clear();
	//int rnd = Util::GetRandom() % DA.size();
	int rnd = 0;
	sv_one = DA[rnd];
	BOOST_LOG_TRIVIAL(debug) << "Connect to " << get<0>(sv_one) << ":" << get<3>(sv_one);
	data_result = GetDataFromUrl(get<1>(sv_one), get<3>(sv_one), "/tor/status-vote/current/consensus");
	if (data_result.empty())
	{
		BOOST_LOG_TRIVIAL(error) << "The file Consensus was not received!";
		return false;
	}
	data_consensus = parser.ParsString(data_result, "\n");
	return data_consensus.size()!=0;
}

void TorLib::LogErr(const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::LogErr";
	if (err) {
		BOOST_LOG_TRIVIAL(error) << err.message();
		error_last_operation = true;
	}
	operation_completed = true;
}

TorLib::~TorLib()
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::~TorLib";
}
bool TorLib::Close()
{	
	BOOST_LOG_TRIVIAL(debug) << "TorLib::Close";
	net_connect.release();
	onion_routers.clear();
	stream_host.clear();
	stream_port=0;
	n_stream=0;
	data_consensus.clear();
	data_result.clear();
	return true;
} 

bool TorLib::CreateNtor(int n_node, ConnectFunction connectFunc)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateNtor ";
	operation_completed = false;

	Cell circuit_node(circuit_id, cell_command::create2);
	circuit_node.Append(static_cast<u16>(2)); // ntor type
	circuit_node.Append(static_cast<u16>(84));  // ntor onion skin length
	// Generating keys
	onion_routers[n_node]->GeneratPairKeys();
	// Add ntor onion skin
	circuit_node.Append(onion_routers[n_node]->identity);
	circuit_node.Append(onion_routers[n_node]->ntor_onion_key);
	circuit_node.Append(onion_routers[n_node]->GetPublicKey(), onion_routers[n_node]->GetPublicKeySize());
	net_connect->WriteCell(circuit_node, boost::bind(&TorLib::ReadCNtor, this,
		n_node, connectFunc, pl::error));
	while (!operation_completed) io_service.poll_one();
	return !error_last_operation;
}
bool TorLib::CreateExtendNtor(int n_node, ConnectFunction connectFunc)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateExtendNtor";
	operation_completed = false;
	RelayCell circuit_node(circuit_id, 0, cell_command::relay_early, cell_command::relay_extend2);
	circuit_node.Append(static_cast<u8>(2)); // 2x NSPEC
	// NSPEC IPv4 (4 bytes) + port (2 bytes)
	circuit_node.Append(static_cast<u8>(link_specifier_type::ipv4));
	circuit_node.Append(static_cast<u8>(6));
	circuit_node.Append(net::ip::address_v4::from_string(onion_routers[n_node]->ip.c_str()).to_uint());
	circuit_node.Append(static_cast<u16>(onion_routers[n_node]->or_port));
	// NSPEC identity_fingerprint (20 bytes)
	circuit_node.Append(static_cast<u8>(link_specifier_type::legacy_id));
	circuit_node.Append(static_cast<u8>(20));
	circuit_node.Append(onion_routers[n_node]->identity);

	circuit_node.Append(static_cast<u16>(2));// HTYPE
	circuit_node.Append(static_cast<u16>(84));// HLEN

	onion_routers[n_node]->GeneratPairKeys();// Generating keys
	// HDATA
	circuit_node.Append(onion_routers[n_node]->identity);
	circuit_node.Append(onion_routers[n_node]->ntor_onion_key);
	circuit_node.Append(onion_routers[n_node]->GetPublicKey(), onion_routers[n_node]->GetPublicKeySize());	

	onion_routers[n_node]->SetKeyMaterial(onion_routers[n_node-1]->key_material.data());
	onion_routers[n_node]->Encrypt(circuit_node);

	net_connect->WriteCell(circuit_node,
		boost::bind(&TorLib::ReadExtendNtor, this, n_node, connectFunc, pl::error));

	while (!operation_completed) io_service.poll_one();
	return !error_last_operation;
}

void TorLib::ReadExtendNtor(int n_node, ConnectFunction connectFunc, const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ReadExtendNtor";
	TEST_ERR(err, connectFunc);
	shared_ptr<Cell> node(new Cell());
	net_connect->ReadCell(node, boost::bind(&TorLib::CreateExtendNtorComplete,
		this, n_node, connectFunc, node, pl::error));
}

void TorLib::CreateExtendNtorComplete(int n_node, ConnectFunction connectFunc,
	shared_ptr<Cell> node, const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateExtendNtorComplete";
	TEST_ERR(err, connectFunc);
	shared_ptr<RelayCell> relay_node(new RelayCell(*node));
	onion_routers[n_node]->Decrypt(*relay_node);
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateExtendNtorComplete Command=" << static_cast<unsigned int>(relay_node->GetCommand());
	sys::error_code loc_err;
	if (onion_routers[n_node]->GeneratKeyMaterial(*relay_node)) loc_err = err;
	else loc_err = net::error::bad_descriptor;
	net::post(net::detail::bind_handler(connectFunc, loc_err));
}

void TorLib::ReadCNtor(int n_node, ConnectFunction connectFunc,
	const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::ReadCNtor";
	TEST_ERR(err, connectFunc);
	shared_ptr<Cell> node(new Cell());
	net_connect->ReadCell(node, boost::bind(&TorLib::CreateNtorComplete,
		this, n_node, connectFunc, node, pl::error));
}

void TorLib::CreateNtorComplete(int n_node, ConnectFunction connectFunc,
	shared_ptr<Cell> node, const sys::error_code& err)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateNtorComplete";
	TEST_ERR(err, connectFunc);
	BOOST_LOG_TRIVIAL(debug) << "TorLib::CreateNtorComplete Command=" << static_cast<unsigned int>(node->GetCommand());
	sys::error_code loc_err;
	if (onion_routers[n_node]->GeneratKeyMaterial(node)) loc_err = err;
	else loc_err = net::error::bad_descriptor;
	net::post(net::detail::bind_handler(connectFunc, loc_err));
}

/*-
 * Copyright (c) 2021, Zano project, https://zano.org/
 * Copyright (c) 2021, Mikhail Butolin, bml505@hotmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of this program nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


bool TorLib::SendNodeInfo(ConnectFunction connectFunc)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::SendNodeInfo";
	operation_completed = false;

	net_connect->SendVersion(boost::bind(&TorLib::LogErr, this, pl::error));

	while (!operation_completed) io_service.poll_one();
	if (error_last_operation) return false;

	operation_completed = false;
	unc loc_host[] = { 0xc0, 0xa8, 0x01, 0x01 }; // Nobody seems to care.
	long rem_host = net_connect->GetEndpointLong();		

	Cell cell_Info(0, cell_command::netinfo);
	cell_Info.Append(static_cast<uint32_t>(time(0)));   // Timestamp
	cell_Info.Append(static_cast<unc>(0x04)); // Type (host)
	cell_Info.Append(static_cast<unc>(0x04)); // Address Length  
	cell_Info.Append(reinterpret_cast<unc*>(&rem_host), 4); // Address
	cell_Info.Append(static_cast<unc>(0x01)); // Address Count
	cell_Info.Append(static_cast<unc>(0x04)); // Type (ipv4)
	cell_Info.Append(static_cast<unc>(0x04));

	net_connect->WriteCell(cell_Info,	boost::bind(&TorLib::LogErr, this, pl::error));	
	while (!operation_completed) io_service.poll_one();
	return !error_last_operation;
}

string TorLib::GetDataFromUrl(const string host, const int port, const string target)
{
	BOOST_LOG_TRIVIAL(debug) << "TorLib::GetDataFromUrl " << host << ":" << port <<" target="<<target;
	// The io_context is required for all I/O
	net::io_context ioc;
	// These objects perform our I/O
	tcp::resolver resolver(ioc);
	beast::tcp_stream stream(ioc);
	// Look up the domain name
	auto const results = resolver.resolve(host, std::to_string(port));
	// Make the connection on the IP address we get from a lookup
	stream.connect(results);
	// Set up an HTTP GET request message
	http::request<http::string_body> req{ http::verb::get, target, 11 };
	req.set(http::field::host, host);
	req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
	// Send the HTTP request to the remote host
	//http::async_write();
	http::write(stream, req);
	// This buffer is used for reading and must be persisted
	beast::flat_buffer buffer;
	// Declare a container to hold the response
	http::response<http::dynamic_body> res;
	// Receive the HTTP response
	http::read(stream, buffer, res);
	// Gracefully close the socket
	beast::error_code ec;
	stream.socket().shutdown(tcp::socket::shutdown_both, ec);
	// not_connected happens sometimes
	// so don't bother reporting it.
	//
	if (ec && ec != beast::errc::not_connected)
	{
		BOOST_LOG_TRIVIAL(error) << ec.value();
		return "";
	}
	std::string ret_str = boost::beast::buffers_to_string(res.body().data());

	return ret_str;
}
