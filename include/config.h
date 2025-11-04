//
// Created by X on 2025/11/1.
//

#ifndef LITECHAT_CONFIG_H
#define LITECHAT_CONFIG_H
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <algorithm>
#include <cctype>

inline std::string trim(const std::string& str)
{
    const size_t first=str.find_first_not_of(" \t\r\n");

    if (std::string::npos ==first)
    {
        return str;
    }

    const size_t last=str.find_last_not_of(" \t\r\n");
    return str.substr(first,(last-first+1));
}

inline std::map<std::string,std::string>load_env(const std::string & filename=".env")
{
    std::map<std::string,std::string>env_vars;
    std::ifstream file(filename);
    std::string line;

    if (!file.is_open())
    {
        std::cerr<<"Error: Could not open .env file: "<<filename<<std::endl;
        return  env_vars;
    }

    while (std::getline(file,line))
    {
        if (line.empty()||line[0]=='#')
        {
            continue;
        }

        size_t delimiter_pos=line.find('=');
        if (delimiter_pos!=std::string::npos)
        {
            std::string key=line.substr(0,delimiter_pos);
            std::string value=line.substr(delimiter_pos+1);

            key=trim(key);
            value=trim(value);

            env_vars[key]=value;
        }
    }

    std::cout<<"Successfully loaded " <<env_vars.size()<<" environment variables from " <<filename<< std::endl;
    return env_vars;
}
#endif //LITECHAT_CONFIG_H