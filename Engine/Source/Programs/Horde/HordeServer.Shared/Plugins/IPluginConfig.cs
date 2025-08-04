// Copyright Epic Games, Inc. All Rights Reserved.

using HordeServer.Acls;

namespace HordeServer.Plugins
{
	/// <summary>
	/// Interface for plugin extensions to the global config object
	/// </summary>
	public interface IPluginConfig
	{
		/// <summary>
		/// Called to fixup a plugin's configuration after deserialization
		/// </summary>
		/// <param name="configOptions">Options for configuring the plugin</param>
		Task PostLoadAsync(PluginConfigOptions configOptions);
	}

	/// <summary>
	/// Options passed to <see cref="IPluginConfig.PostLoadAsync(PluginConfigOptions)"/>
	/// </summary>
	public record class PluginConfigOptions(ConfigVersion Version, IEnumerable<IPluginConfig> Plugins, AclConfig ParentAcl);

	/// <summary>
	/// Empty implementation of <see cref="IPluginConfig"/>
	/// </summary>
	public sealed class EmptyPluginConfig : IPluginConfig
	{
		/// <inheritdoc/>
		public Task PostLoadAsync(PluginConfigOptions configOptions)
		{
			return Task.CompletedTask;
		}
	}
}
