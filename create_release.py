import os
import sys
from shutil import rmtree
from urllib.request import Request, urlopen, urlretrieve
import json
try:
	import requests
except ImportError:
	print("you need to do 'python -m pip install requests'");
	exit(0);
import zipfile
import io
import time
from datetime import date
import tarfile
import subprocess
import argparse
import re
from pathlib import Path

# function to get a var from version.inc
def get_cmake_var(filepath, var_name):
	pattern = rf'set\(\s*{var_name}\s+"([^"]+)"\s*\)'

	with open(filepath, "r") as f:
		content = f.read()

	match = re.search(pattern, content)
	if match:
		return match.group(1)
	return None

parser = argparse.ArgumentParser(
	description="Create release folders without overwriting existing ones."
)

parser.add_argument("branch_name", nargs="?", default="rc", help="Branch name")
parser.add_argument("prefix", nargs="?", default="rc", help="Prefix name")
parser.add_argument("date", nargs="?", default="", help="Artifact compile date")

args = parser.parse_args()

branch_name = args.branch_name
prefix = args.prefix
first_day = args.date;
repo = get_cmake_var("version.inc", "SLIC3R_GITHUB")
program_name = get_cmake_var("version.inc", "SLIC3R_APP_KEY")
token = None

path_7zip = r"C:\Program Files\7-Zip\7z.exe"
# github classic personal access token, works with [gist, repo, workflow] permissions, should be something like "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"
github_auth_token = ""
# try to fill github_auth_token via file
githubtoken_file = Path("githubtoken.ini")
if githubtoken_file.exists():
	with open(githubtoken_file) as f:
		for line in f:
			if line.startswith("token="):
				github_auth_token = line.strip().split("=", 1)[1];
				print(f"file githubtoken.ini found, using token:'{github_auth_token}'");
				break
else:
	print(f"file githubtoken.ini not found, using hardcoded token:'{github_auth_token}'");
	if(not github_auth_token):
		print("For this scipt to work, please create a 'githubtoken.ini' file, and make at least a line with 'token=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'");

def get_version():
	settings_stream = open("./version.inc", mode="r", encoding="utf-8");
	lines = settings_stream.read().splitlines();
	version = "0.0.0.0";
	tag="";
	for line in lines:
		if("set(SLIC3R_RC_VERSION_DOTS" in line):
			print("found1: " +line)
			elems = line.split("\"");
			version = elems[1];
		if("set(SLIC3R_VERSION_TAG" in line):
			print("found2: " +line)
			elems = line.split("\"");
			tag = elems[1];
	if("+" in tag):
		elems = tag.split("+");
		tag = elems[0];
	return version+tag;

found_win = False; 
found_win_msi = False; 
found_linux = False; 
found_linux_appimage_gtk2 = False; 
found_linux_appimage_gtk3 = False; 
found_macos = False; 
found_macos_arm = False;

# return True if he want to cntue new artifacts
def handle_artifact(json_artifact):
	global found_win
	global found_win_msi
	global found_linux
	global found_linux_appimage_gtk2
	global found_linux_appimage_gtk3
	global found_macos
	global found_macos_arm
	global first_day
	
	if json_artifact["workflow_run"]["head_branch"] == branch_name:
		if first_day == "":
			print("encounter the first " + branch_name + " at " + json_artifact["created_at"][:10]);
			first_day = json_artifact["created_at"][:10];
		if json_artifact["created_at"][:10] == first_day:
			print("Next artifact: " + json_artifact["name"]);
		elif json_artifact["created_at"][:10] > first_day:
			print("Ignored artifact (bad day): ("+json_artifact["name"] + "  @ "+json_artifact["created_at"][:10]+")");
			return True
		else:
			print(f"End of {branch_name} artifacts (date too far away). Closing");
			print("("+json_artifact["name"] + "  @ "+json_artifact["created_at"][:10]+")");
			return False;
		if json_artifact["name"] == prefix + "_" +program_name + "-win64" and not found_win:
			found_win = True;
			print("Found win64 artifact");
			print("ask for: "+json_artifact["archive_download_url"]);
			resp = requests.get(json_artifact["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("win: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content))
			base_name = release_path+"/"+program_name+"_"+version+"_win64_"+date_str;
			z.extractall(base_name);
			try:
				ret = subprocess.check_output([path_7zip, "a", "-tzip", base_name+".zip", base_name]);
			except:
				print("Failed to zip the win directory, do it yourself");
		if json_artifact["name"] ==  prefix + "_" +program_name + "-win64.msi" and not found_win_msi:
			found_win_msi = True;
			print("Found win64 msi artifact");
			print("ask for: "+json_artifact["archive_download_url"]);
			resp = requests.get(json_artifact["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("win: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content))
			z.extractall(release_path);
			os.rename(release_path+"/"+prefix + "_" +program_name+"-win64.msi", release_path+"/"+program_name+"_"+version+"_win64_"+date_str+".msi");
		if json_artifact["name"] == prefix + "_"+program_name+"-macOS-intel.dmg" and not found_macos:
			found_macos = True;
			print("Found macos-intel artifact");
			print("ask for: "+json_artifact["archive_download_url"]);
			resp = requests.get(json_artifact["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("macos: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			os.rename(release_path+"/"+program_name+"-macOS-intel.dmg", release_path+"/"+program_name+"_"+version+"_macos_"+date_str+".dmg");
		if json_artifact["name"] == prefix + "_"+program_name+"-macOS-arm.dmg" and not found_macos_arm:
			found_macos_arm = True;
			print("Found macos-arm artifact");
			print("ask for: "+json_artifact["archive_download_url"]);
			resp = requests.get(json_artifact["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("macos-arm: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			os.rename(release_path+"/"+program_name+"-macOS-arm.dmg", release_path+"/"+program_name+"_"+version+"_macos_arm_"+date_str+".dmg");
		if json_artifact["name"] == prefix + "_"+program_name+"-linux-x64-GTK2.AppImage" and not found_linux_appimage_gtk2:
			found_linux_appimage_gtk2 = True;
			print("Found ubuntu GTK2 artifact");
			print("ask for: "+json_artifact["archive_download_url"]);
			resp = requests.get(json_artifact["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("appimage: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			os.rename(release_path+"/"+program_name+"-linux-x64-GTK2.AppImage", release_path+"/"+program_name+"-ubuntu_22.04-gtk2-" + version + ".AppImage");
		if json_artifact["name"] == prefix + "_"+program_name+"-linux-x64-GTK3.AppImage" and not found_linux_appimage_gtk3:
			found_linux_appimage_gtk3 = True;
			print("Found ubuntu GTK3 artifact");
			print("ask for: "+json_artifact["archive_download_url"]);
			resp = requests.get(json_artifact["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("appimage: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			os.rename(release_path+"/"+program_name+"-linux-x64-GTK3.AppImage", release_path+"/"+program_name+"-ubuntu_22.04-" + version + ".AppImage");
		if json_artifact["name"] == prefix + "_"+program_name+"-linux-x64-GTK3.tgz" and not found_linux:
			found_linux = True;
			print("Found ubuntu GTK3 archive artifact");
			print("ask for: "+json_artifact["archive_download_url"]);
			resp = requests.get(json_artifact["archive_download_url"], headers={'Authorization': 'token ' + github_auth_token,}, allow_redirects=True);
			print("appimage: " +str(resp));
			z = zipfile.ZipFile(io.BytesIO(resp.content));
			z.extractall(release_path);
			base_path = release_path+"/"+program_name+"_" + version + "_linux64_" + date_str;
			os.rename(release_path+"/"+program_name+"-linux-x64-GTK3.tgz", base_path+".tgz");
			# try:
				# subprocess.check_output([path_7zip, "a", "-tzip", base_path+".tar.zip", base_path+".tar"]);
				# os.remove(base_path+".tar");
			# except:
				# with zipfile.ZipFile(base_path+"_bof.tar.zip", 'w') as myzip:
					# myzip.write(base_path+".tar");
	return  not (found_win and found_win_msi and found_linux and found_linux_appimage_gtk2 and found_linux_appimage_gtk3 and found_macos and found_macos_arm);

date_str = date.today().strftime('%y%m%d');
version = get_version();
print("create release for: " + str(version));
if(not os.path.isdir("./releases")):
	os.mkdir("./releases");
base_path = "./releases/"+branch_name+"_release_"+str(version);
release_path = base_path;

# find a directory that either doesn't exist OR is empty
counter = 2;
while os.path.isdir(release_path) and os.listdir(release_path):
	release_path = f"{base_path}_{counter}";
	counter += 1;
if not os.path.isdir(release_path):
	os.mkdir(release_path);

#urllib.urlretrieve ("https://api.github.com/repos/"+repo+"/actions/artifacts", release_path+"artifacts.json");
need_more = True
page = 1
while need_more and page < 10:
	with urlopen("https://api.github.com/repos/"+repo+"/actions/artifacts?page="+str(page)) as f:
		artifacts = json.loads(f.read().decode('utf-8'));
		print("there is "+ str(artifacts["total_count"])+ " artifacts in the repo");
		for entry in artifacts["artifacts"]:
			need_more = handle_artifact(entry);
			if not need_more:
				break;
	page = page + 1;

print("DONT FORGET TO PUSH YOUR MAIN/MASTER BRANCH");
