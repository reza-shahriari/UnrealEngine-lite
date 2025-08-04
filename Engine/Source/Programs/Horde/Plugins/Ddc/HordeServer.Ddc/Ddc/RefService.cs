// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Horde.Storage;
using EpicGames.Horde.Storage.Nodes;
using EpicGames.Serialization;

namespace HordeServer.Ddc
{
	class RefService : IRefService
	{
		readonly IStorageClient _storageClient;
		readonly IReferenceResolver _referenceResolver;
		readonly IBlobService _blobService;

		public RefService(IStorageClient storageClient, IReferenceResolver referenceResolver, IBlobService blobService)
		{
			_storageClient = storageClient;
			_referenceResolver = referenceResolver;
			_blobService = blobService;
		}

		static RefName GetRefName(BucketId bucketId, RefId refId) => $"{bucketId}/{refId}";

		public async Task<bool> DeleteAsync(NamespaceId ns, BucketId bucket, RefId key, CancellationToken cancellationToken)
		{
			IStorageNamespace storageNamespace = _storageClient.GetNamespace(ns);

			RefName refName = GetRefName(bucket, key);
			if (!await storageNamespace.RefExistsAsync(refName, cancellationToken: cancellationToken))
			{
				return false;
			}

			await storageNamespace.RemoveRefAsync(refName, cancellationToken);
			return true;
		}

		public async Task<bool> ExistsAsync(NamespaceId ns, BucketId bucket, RefId key, CancellationToken cancellationToken)
		{
			IStorageNamespace storageNamespace = _storageClient.GetNamespace(ns);
			return await storageNamespace.RefExistsAsync(GetRefName(bucket, key), cancellationToken: cancellationToken);
		}

		public async Task<(ContentId[], BlobId[])> FinalizeAsync(NamespaceId ns, BucketId bucket, RefId key, BlobId blobHash, CancellationToken cancellationToken)
		{
			IStorageNamespace storageNamespace = _storageClient.GetNamespace(ns);

			BlobAlias? blobAlias = await storageNamespace.FindAliasAsync(BlobService.GetAlias(blobHash), cancellationToken);
			if (blobAlias == null)
			{
				throw new BlobNotFoundException(ns, blobHash);
			}

			IBlobRef blobHandle = blobAlias.Target;
			using BlobData blobContents = await blobHandle.ReadBlobDataAsync(cancellationToken);
			CbObject payload = new CbObject(blobContents.Data);

			BlobId[] referencedBlobs = Array.Empty<BlobId>();

			ContentId[] missingReferences = Array.Empty<ContentId>();
			BlobId[] missingBlobs = Array.Empty<BlobId>();
			bool hasReferences = HasAttachments(payload);
			if (hasReferences)
			{
				try
				{
					referencedBlobs = await _referenceResolver.GetReferencedBlobsAsync(ns, payload, cancellationToken: cancellationToken).ToArrayAsync(cancellationToken);
				}
				catch (PartialReferenceResolveException e)
				{
					missingReferences = e.UnresolvedReferences.ToArray();
				}
				catch (ReferenceIsMissingBlobsException e)
				{
					missingBlobs = e.MissingBlobs.ToArray();
				}
			}

			if (missingReferences.Length == 0 && missingBlobs.Length == 0)
			{
				// TODO: We resolved all these blobs above... Need to just have GetReferencedBlobs just return the appropriate handles directly.
				RefName refName = GetRefName(bucket, key);

				IHashedBlobRef<DdcRefNode> refNodeRef;
				await using (IBlobWriter writer = storageNamespace.CreateBlobWriter(refName))
				{
					DdcRefNode refNode = new DdcRefNode(blobHash.AsIoHash());
					refNode.References.Add(HashedBlobRef.Create(blobHash.AsIoHash(), blobHandle));
					foreach (BlobId referencedBlob in referencedBlobs)
					{
						BlobAlias? alias = await storageNamespace.FindAliasAsync(BlobService.GetAlias(referencedBlob), cancellationToken);
						refNode.References.Add(HashedBlobRef.Create(referencedBlob.AsIoHash(), alias!.Target));
					}
					refNodeRef = await writer.WriteBlobAsync(refNode, cancellationToken);
				}

				await storageNamespace.AddRefAsync(refName, refNodeRef, cancellationToken: cancellationToken);
			}

			return (missingReferences, missingBlobs);
		}

		private bool HasAttachments(CbObject payload)
		{
			bool FieldHasAttachments(CbField field)
			{
				if (field.IsObject())
				{
					bool hasAttachment = HasAttachments(field.AsObject());
					if (hasAttachment)
					{
						return true;
					}
				}

				if (field.IsArray())
				{
					foreach (CbField subField in field.AsArray())
					{
						bool hasAttachment = FieldHasAttachments(subField);
						if (hasAttachment)
						{
							return true;
						}
					}
				}

				return field.IsAttachment();
			}

			return payload.Any(FieldHasAttachments);
		}

		public async Task<(RefRecord, BlobContents?)> GetAsync(NamespaceId ns, BucketId bucket, RefId key, string[] fields, bool doLastAccessTracking, CancellationToken cancellationToken)
		{
			IStorageNamespace storageNamespace = _storageClient.GetNamespace(ns);

			DdcRefNode? node = await storageNamespace.TryReadRefTargetAsync<DdcRefNode>(GetRefName(bucket, key), cancellationToken: cancellationToken);
			if (node == null)
			{
				throw new RefNotFoundException(ns, bucket, key);
			}

			using BlobData data = await node.References.First(x => x.Hash == node.RootHash).ReadBlobDataAsync(cancellationToken);
			BlobContents contents = new BlobContents(data.Data.ToArray());

			RefRecord record = new RefRecord(ns, bucket, key, DateTime.UtcNow, null, BlobId.FromIoHash(node.RootHash), true);
			return (record, contents);
		}

		public async Task<List<BlobId>> GetReferencedBlobsAsync(NamespaceId ns, BucketId bucket, RefId key, CancellationToken cancellationToken)
		{
			IStorageNamespace storageNamespace = _storageClient.GetNamespace(ns);

			DdcRefNode? node = await storageNamespace.TryReadRefTargetAsync<DdcRefNode>(GetRefName(bucket, key), cancellationToken: cancellationToken);
			if (node == null)
			{
				throw new RefNotFoundException(ns, bucket, key);
			}

			return node.References.Select(x => BlobId.FromIoHash(x.Hash)).ToList();
		}

		public async Task<(ContentId[], BlobId[])> PutAsync(NamespaceId ns, BucketId bucket, RefId key, BlobId blobHash, CbObject payload, CancellationToken cancellationToken)
		{
			await _blobService.PutObjectAsync(ns, payload.GetView().ToArray(), blobHash, cancellationToken);
			return await FinalizeAsync(ns, bucket, key, blobHash, cancellationToken);
		}

		public IAsyncEnumerable<NamespaceId> GetNamespacesAsync(CancellationToken cancellationToken)
		{
			throw new NotImplementedException();
		}

		public Task<long> DropNamespaceAsync(NamespaceId ns, CancellationToken cancellationToken)
		{
			throw new NotImplementedException();
		}

		public Task<long> DeleteBucketAsync(NamespaceId ns, BucketId bucket, CancellationToken cancellationToken)
		{
			throw new NotImplementedException();
		}

		public Task<List<BlobId>> GetReferencedBlobsAsync(NamespaceId ns, BucketId bucket, RefId key, bool ignoreMissingBlobs, CancellationToken cancellationToken)
		{
			throw new NotImplementedException();
		}
	}
}
